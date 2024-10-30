#include <switch.h>
#include <cmath>

#define MSG_LOG(Severity, Format, ...) switch_log_printf(SWITCH_CHANNEL_LOG, Severity, Format, ##__VA_ARGS__)
#define MSG_SESSION_LOG(Session, Severity, Format, ...) switch_log_printf(SWITCH_CHANNEL_SESSION_LOG((Session)), Severity, Format, ##__VA_ARGS__)

#define BUG_AMD_NAME_READ "mod_amd_read"
#define AMD_EVENT_NAME "mod_amd::info"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_amd_load);
SWITCH_MODULE_DEFINITION(mod_amd, mod_amd_load, mod_amd_shutdown, NULL);
SWITCH_STANDARD_APP(amd_start_function);

namespace {

struct amd_params {
    uint32_t initial_silence;
    uint32_t greeting;
    uint32_t after_greeting_silence;
    uint32_t total_analysis_time;
    uint32_t minimum_word_length;
    uint32_t between_words_silence;
    uint32_t maximum_number_of_words;
    uint32_t maximum_word_length;
    uint32_t silence_threshold;
    switch_bool_t silence_not_sure;
};

amd_params globals;

switch_xml_config_item_t config_options[] = {
        SWITCH_CONFIG_ITEM("initial_silence", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.initial_silence, (void *) 2500, NULL, NULL, NULL),
        SWITCH_CONFIG_ITEM("greeting", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.greeting,(void *) 1500, NULL, NULL, NULL),
        SWITCH_CONFIG_ITEM("after_greeting_silence", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.after_greeting_silence, (void *) 800, NULL, NULL, NULL),
        SWITCH_CONFIG_ITEM("total_analysis_time", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.total_analysis_time, (void *) 5000, NULL, NULL, NULL),
        SWITCH_CONFIG_ITEM("min_word_length", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.minimum_word_length, (void *) 100, NULL, NULL, NULL),
        SWITCH_CONFIG_ITEM("between_words_silence", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.between_words_silence, (void *) 50, NULL, NULL, NULL),
        SWITCH_CONFIG_ITEM("maximum_number_of_words", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.maximum_number_of_words, (void *) 3, NULL, NULL, NULL),
        SWITCH_CONFIG_ITEM("maximum_word_length", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.maximum_word_length, (void *)5000, NULL, NULL, NULL),
        SWITCH_CONFIG_ITEM("silence_threshold", SWITCH_CONFIG_INT, CONFIG_RELOADABLE, &globals.silence_threshold, (void *) 256, NULL, NULL, NULL),
        SWITCH_CONFIG_ITEM("silence_not_sure", SWITCH_CONFIG_BOOL, CONFIG_RELOADABLE, &globals.silence_not_sure, SWITCH_FALSE, NULL, NULL, NULL),
        SWITCH_CONFIG_ITEM_END()
};

enum class AmdFrameClassifier { VOICED, SILENCE };
enum class AmdVadState { IN_WORD, IN_SILENCE };

struct amd_vad {
    switch_core_session_t* session;
    switch_channel_t* channel;
    switch_codec_implementation_t read_impl;

    AmdVadState state;
    amd_params params;
    uint32_t frame_ms;
    int32_t sample_count_limit;

    uint32_t silence_duration;
    uint32_t voice_duration;
    uint32_t words;

    bool in_initial_silence;
    bool in_greeting;
};

void amd_fire_event(switch_channel_t *channel) 
{
    switch_event_t* event;
    switch_status_t status = switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, AMD_EVENT_NAME);

    if (status != SWITCH_STATUS_SUCCESS)
        return;

    switch_channel_event_set_data(channel, event);
    switch_event_fire(&event);
}

void do_execute(switch_core_session_t *session, switch_channel_t *channel, const char *name) 
{
    const char* variable = switch_channel_get_variable(channel, name);

    if (!variable) 
        return;

    char* expanded = switch_channel_expand_variables(channel, variable);
    char* app = switch_core_session_strdup(session, expanded);
    switch_assert(app != nullptr);

    char *arg = nullptr;
    bool bg = false;

    for (char* p = app; p && *p; p++) 
    {
        if (*p == ' ' || (*p == ':' && (*(p+1) != ':'))) 
        {
            *p++ = '\0';
            arg = p;
            break;
        }
        else if (*p == ':' && (*(p+1) == ':')) 
        {
            bg = true;
            break;
        }
    }

    if (bg == false && !strncasecmp(app, "perl", 4)) 
        bg = true;

    if (bg) 
        switch_core_session_execute_application_async(session, app, arg);
    else 
        switch_core_session_execute_application(session, app, arg);
}

AmdFrameClassifier classify_frame(uint32_t silence_threshold, const switch_frame_t* frame, const switch_codec_implementation_t* codec) 
{
    const int16_t* audio = reinterpret_cast<const int16_t*>(frame->data);
    double energy = 0.0;

    for (uint32_t i = 0; i < frame->samples; ++i) 
        energy += std::abs(audio[i]);

    uint32_t score = static_cast<uint32_t>(energy / frame->samples);

    return score >= silence_threshold ? AmdFrameClassifier::VOICED : AmdFrameClassifier::SILENCE;
}

switch_bool_t amd_handle_silence_frame(amd_vad *vad, const switch_frame_t *f)
{
    vad->silence_duration += vad->frame_ms;

    if (vad->silence_duration >= vad->params.between_words_silence) 
    {
#ifdef NDEBUG
        if (vad->state != AmdVadState::IN_SILENCE) 
            MSG_SESSION_LOG(vad->session, SWITCH_LOG_DEBUG, "AMD: Changed state to IN_SILENCE\n");
#endif

        vad->state = AmdVadState::IN_SILENCE;
        vad->voice_duration = 0;
    }

    if (vad->in_initial_silence && vad->silence_duration >= vad->params.initial_silence) 
    {
        const char* result = vad->params.silence_not_sure ? "NOT_SURE" : "MACHINE";
#ifdef NDEBUG
        MSG_SESSION_LOG(vad->session, SWITCH_LOG_DEBUG, "AMD: %s (silence_duration: %d, initial_silence: %d, silence_not_sure: %d)\n", result, vad->silence_duration, vad->params.initial_silence, vad->params.silence_not_sure);
#endif

        switch_channel_set_variable(vad->channel, "amd_result", result);
        switch_channel_set_variable(vad->channel, "amd_cause", "INITIAL_SILENCE");
        return SWITCH_TRUE;
    }

    if (vad->silence_duration >= vad->params.after_greeting_silence && vad->in_greeting) 
    {
#ifdef NDEBUG
        MSG_SESSION_LOG(vad->session, SWITCH_LOG_DEBUG, "AMD: HUMAN (silence_duration: %d, after_greeting_silence: %d)\n", vad->silence_duration, vad->params.after_greeting_silence);
#endif
        switch_channel_set_variable(vad->channel, "amd_result", "HUMAN");
        switch_channel_set_variable(vad->channel, "amd_cause", "SILENCE_AFTER_GREETING");
        return SWITCH_TRUE;
    }

    return SWITCH_FALSE;
}

switch_bool_t amd_handle_voiced_frame(amd_vad *vad, const switch_frame_t *f)
{
    vad->voice_duration += vad->frame_ms;

    if (vad->voice_duration >= vad->params.minimum_word_length && vad->state == AmdVadState::IN_SILENCE) 
    {
        vad->words++;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG, "AMD: Word detected (words: %d)\n", vad->words);
        vad->state = AmdVadState::IN_WORD;
    }

    if (vad->voice_duration >= vad->params.maximum_word_length) 
    {
#ifdef NDEBUG
        MSG_SESSION_LOG(vad->session, SWITCH_LOG_DEBUG, "AMD: MACHINE (voice_duration: %d, maximum_word_length: %d)\n", vad->voice_duration, vad->params.maximum_word_length);
#endif
        switch_channel_set_variable(vad->channel, "amd_result", "MACHINE");
        switch_channel_set_variable(vad->channel, "amd_cause", "MAX_WORD_LENGTH");
        return SWITCH_TRUE;
    }

    if (vad->words >= vad->params.maximum_number_of_words) 
    {
#ifdef NDEBUG
        MSG_SESSION_LOG(vad->session, SWITCH_LOG_DEBUG, "AMD: MACHINE (words: %d, maximum_number_of_words: %d)\n", vad->words, vad->params.maximum_number_of_words);
#endif
        switch_channel_set_variable(vad->channel, "amd_result", "MACHINE");
        switch_channel_set_variable(vad->channel, "amd_cause", "MAX_WORDS");
        return SWITCH_TRUE;
    }

    if (vad->in_greeting && vad->voice_duration >= vad->params.greeting) 
    {
#ifdef NDEBUG
        MSG_SESSION_LOG(vad->session, SWITCH_LOG_DEBUG, "AMD: MACHINE (voice_duration: %d, greeting: %d)\n", vad->voice_duration, vad->params.greeting);
#endif
        switch_channel_set_variable(vad->channel, "amd_result", "MACHINE");
        switch_channel_set_variable(vad->channel, "amd_cause", "LONG_GREETING");
        return SWITCH_TRUE;
    }

    if (vad->voice_duration >= vad->params.minimum_word_length) 
    {
#ifdef NDEBUG
        if (vad->silence_duration) 
             MSG_SESSION_LOG(vad->session, SWITCH_LOG_DEBUG, "AMD: Detected Talk, previous silence duration: %dms\n", vad->silence_duration);
#endif
        vad->silence_duration = 0;
    }

    if (vad->voice_duration >= vad->params.minimum_word_length && !vad->in_greeting) 
    {
        if (vad->silence_duration)
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG, "AMD: Before Greeting Time (silence_duration: %d, voice_duration: %d)\n", vad->silence_duration, vad->voice_duration);

        vad->in_initial_silence = false;
        vad->in_greeting = true;
    }

    return SWITCH_FALSE;
}

switch_bool_t amd_read_audio_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    amd_vad *vad = reinterpret_cast<amd_vad*>(user_data);

    switch (type) 
    {
        case SWITCH_ABC_TYPE_INIT:
            switch_core_session_get_read_impl(vad->session, &vad->read_impl);
            if (vad->params.total_analysis_time) 
                vad->sample_count_limit = (vad->read_impl.actual_samples_per_second / 1000) * vad->params.total_analysis_time;
            break;

        case SWITCH_ABC_TYPE_CLOSE:
            if (switch_channel_ready(vad->channel)) 
            {
                const char* result = switch_channel_get_variable(vad->channel, "amd_result");
                if (result == NULL) 
                {
                    result = "NOT_SURE";
                    MSG_SESSION_LOG(vad->session, SWITCH_LOG_WARNING, "variable amd_result not defined. set amd_result=NOT_SURE\n");
                    switch_channel_set_variable(vad->channel, "amd_result", "NOT_SURE");
                    switch_channel_set_variable(vad->channel, "amd_cause", "TOO_LONG");
                }

                amd_fire_event(vad->channel);

                if (!strcasecmp(result, "MACHINE"))
                    do_execute(vad->session, vad->channel, "mod_amd_on_machine");
                else if (!strcasecmp(result, "HUMAN"))
                    do_execute(vad->session, vad->channel, "mod_amd_on_human");
                else
                    do_execute(vad->session, vad->channel, "mod_amd_on_not_sure");
            }
            else 
            {
                if (!switch_channel_get_variable(vad->channel, "amd_result")) 
                {
                    MSG_SESSION_LOG(vad->session, SWITCH_LOG_DEBUG, "variable amd_result not defined. set amd_result=CANCEL\n");
                    switch_channel_set_variable(vad->channel, "amd_result", "CANCEL");
                    switch_channel_set_variable(vad->channel, "amd_cause", "CANCEL");
                }
            }

#ifdef NDEBUG 
            MSG_SESSION_LOG(vad->session, SWITCH_LOG_DEBUG, "AMD: close\n");
#endif
            break;

        case SWITCH_ABC_TYPE_READ_PING:
        {
            uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
            switch_frame_t read_frame = {0} ;
            read_frame.data = data;
            read_frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

            switch_status_t status = switch_core_media_bug_read(bug, &read_frame, SWITCH_FALSE);

            if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) 
                return SWITCH_TRUE;

            if (vad->sample_count_limit) 
            {
                vad->sample_count_limit -= read_frame.samples;

                if (vad->sample_count_limit <= 0) 
                {
                    switch_channel_set_variable(vad->channel, "amd_result", "NOT_SURE");
                    switch_channel_set_variable(vad->channel, "amd_cause", "TOO_LONG");
                    return SWITCH_FALSE;
                }
            }

            vad->frame_ms = 1000 / (vad->read_impl.actual_samples_per_second / read_frame.samples);

            switch (classify_frame(vad->params.silence_threshold, &read_frame, &vad->read_impl)) 
            {
                case AmdFrameClassifier::SILENCE:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG, "AMD: Silence\n");
                    if (amd_handle_silence_frame(vad, &read_frame) == SWITCH_TRUE)
                        return SWITCH_FALSE;
                    break;

                case AmdFrameClassifier::VOICED:
                default:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(vad->session), SWITCH_LOG_DEBUG, "AMD: Voiced\n");
                    if (amd_handle_voiced_frame(vad, &read_frame) == SWITCH_TRUE)
                        return SWITCH_FALSE;
                    break;
            }
            break;
        }
        default:
            break;
    }

    return SWITCH_TRUE;
}

}

SWITCH_MODULE_LOAD_FUNCTION(mod_amd_load)
{
    memset(&globals, 0, sizeof(globals));
    if (switch_xml_config_parse_module_settings("mod_amd.conf", SWITCH_FALSE, config_options) != SWITCH_STATUS_SUCCESS) 
    {
        MSG_LOG(SWITCH_LOG_ERROR, "Failed to load config\n");
        return SWITCH_STATUS_FALSE;
    }

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);
    switch_application_interface_t *app_interface;
    SWITCH_ADD_APP(app_interface, "mod_amd", "Voice activity detection (non-blocking)", "Asterisk's AMD (Non-blocking)", amd_start_function, NULL, SAF_NONE);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_shutdown)
{
    switch_xml_config_cleanup(config_options);
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(amd_start_function)
{
    if (!session)
        return;

    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (!switch_channel_media_up(channel) || !switch_core_session_get_read_codec(session)) 
    {
        MSG_SESSION_LOG(session, SWITCH_LOG_ERROR, "Can not record session.  Media not enabled on channel\n");
        return;
    }

    amd_vad* vad = reinterpret_cast<amd_vad*>(switch_core_session_alloc(session, sizeof(amd_vad)));
    vad->params = globals;
    vad->channel = channel;
    vad->session = session;
    vad->state = AmdVadState::IN_WORD;
    vad->sample_count_limit = 0;
    vad->silence_duration = 0;
    vad->voice_duration = 0;
    vad->frame_ms = 0;
    vad->in_initial_silence = true;
    vad->in_greeting = false;
    vad->words = 0;

    // parsing arguments

    char *arg = (char *) data;
    char delim = ' ';

    if (!zstr(arg) && *arg == '^' && *(arg+1) == '^') 
    {
        arg += 2;
        delim = *arg++;
    }

    if (arg) 
    {
        int x, argc;
        char *argv[10] = { 0 };
        char *param[2] = { 0 };

        arg = switch_core_session_strdup(session, arg);
        argc = switch_split(arg, delim, argv);

        for (x = 0; x < argc; x++) 
        {
            if (switch_separate_string(argv[x], '=', param, switch_arraylen(param)) == 2) 
            {
                int value = 0;
                if(!strcasecmp(param[0], "initial_silence")) 
                {
                    if ((value = atoi(param[1])) > 0)
                        vad->params.initial_silence = value;
                }
                else if (!strcasecmp(param[0], "greeting")) 
                {
                    if ((value = atoi(param[1])) > 0) 
                        vad->params.greeting = value;
                } 
                else if (!strcasecmp(param[0], "after_greeting_silence")) 
                {
                    if ((value = atoi(param[1])) > 0)
                        vad->params.after_greeting_silence = value;
                } 
                else if (!strcasecmp(param[0], "total_analysis_time")) 
                {
                    if ((value = atoi(param[1])) > 0)
                        vad->params.total_analysis_time = value;
                } 
                else if (!strcasecmp(param[0], "min_word_length")) 
                {
                    if ((value = atoi(param[1])) > 0)
                        vad->params.minimum_word_length = value;
                } 
                else if (!strcasecmp(param[0], "between_words_silence")) 
                {
                    if ((value = atoi(param[1])) > 0)
                        vad->params.between_words_silence = value;
                }
                else if (!strcasecmp(param[0], "maximum_number_of_words")) 
                {
                    if ((value = atoi(param[1])) > 0)
                        vad->params.maximum_number_of_words = value;
                } 
                else if (!strcasecmp(param[0], "maximum_word_length")) 
                {
                    if ((value = atoi(param[1])) > 0) 
                        vad->params.maximum_word_length = value;
                } 
                else if (!strcasecmp(param[0], "silence_threshold")) 
                {
                    if ((value = atoi(param[1])) > 0)
                        vad->params.silence_threshold = value;
                }
                else if (!strcasecmp(param[0], "silence_not_sure")) 
                {
                    vad->params.silence_not_sure = switch_true(param[1]);
                    value = 1;
                }

                if (value < 1)
                    MSG_SESSION_LOG(session, SWITCH_LOG_ERROR, "AMD: Invalid [%s]=[%s]; Value must be positive integer only!\n", param[0], param[1]);
            } 
            else 
            {
                MSG_SESSION_LOG(session, SWITCH_LOG_ERROR, "AMD: Ignore argument [%s]\n", argv[x]);
            }
        }
    }

    switch_media_bug_t *bug = NULL;

    if (switch_core_media_bug_add(session, BUG_AMD_NAME_READ, NULL, amd_read_audio_callback, vad, 0, SMBF_READ_STREAM | SMBF_READ_PING, &bug) != SWITCH_STATUS_SUCCESS ) 
    {
        MSG_SESSION_LOG(session, SWITCH_LOG_ERROR, "Can not add media bug.  Media not enabled on channel\n");
        return;
    }
}
