# Asterisk app_amd for FreeSWITCH

This is an implementation of Asterisk's answering machine detection (voice activity detection) for FreeSWITCH.

# Building 

To build module as a part of FreeSwitch source tree perform following steps:

- Add it as a git submodule `git submodule add --name mod_amd https://github.com/silviucpp/mod_amd src/mod/applications/mod_amd`
- Add `src/mod/applications/mod_amd/Makefile` to `AC_CONFIG_FILES` of `configure.ac`
- Add `applications/mod_amd` to `modules.conf`
- Build entire FreeSwitch project or module only

# Configuration

Add into FreeSwitch installation, in **conf/autoload_configs/mod_amd.conf.xml** the following content or the template file you can find into `conf` folder. 

### Available options:

- `initial_silence` - is maximum initial silence duration before greeting. If this is exceeded, the result is detection as a `MACHINE`.
- `greeting` - is the maximum length of a greeting. If this is exceeded, the result is detection as a `MACHINE`.
- `after_greeting_silence` - is the silence after detecting a greeting. If this is exceeded, the result is detection as a `HUMAN`.
- `total_analysis_time` - is the maximum time allowed for the algorithm to decide on whether the audio represents a `HUMAN`, or a `MACHINE`.
- `maximum_word_length` - is the maximum duration of a word to accept. If exceeded, then the result is detection as a `MACHINE`.
- `min_word_length` - is the minimum duration of Voice considered to be a word.
- `between_words_silence` - is the minimum duration of silence after a word to consider the audio that follows to be a new word.
- `maximum_number_of_words` - is the maximum number of words in a greeting If this is exceeded, then the result is detection as a `MACHINE`.
- `silence_threshold` - the average level of noise from 0 to 32767 which if not exceeded, should be considered silence.
- `silence_not_sure` - enable or disable the `NOT_SURE` result. In case is disabled `MACHINE` is sent instead.

### Example:

```xml
<configuration name="mod_amd.conf" description="mod_amd Configuration">
  <settings>
    <param name="silence_threshold" value="256"/>
    <param name="maximum_word_length" value="5000"/>
    <param name="maximum_number_of_words" value="3"/>
    <param name="between_words_silence" value="50"/>
    <param name="min_word_length" value="100"/>
    <param name="total_analysis_time" value="5000"/>
    <param name="after_greeting_silence" value="800"/>
    <param name="greeting" value="1500"/>
    <param name="initial_silence" value="2500"/>
    <param name="silence_not_sure" value="no"/>
  </settings>
</configuration>
```

# Variables

After the AMD execution the `mod_amd::info` event is triggered and the variable `amd_result` and `amd_cause` will be set.

The variable `amd_result` will contain one of the following values:

- `NOT_SURE` - take this value if `total_analysis_time` is over and decision could not be made and `silence_not_sure` is on.
- `HUMAN` - if a human is detected.
- `MACHINE` - if a human is not detected.

The variable `amd_cause` will return one of the following values:

- `INITIAL_SILENCE` (MACHINE)
- `SILENCE_AFTER_GREETING` (HUMAN)
- `MAX_WORD_LENGTH` (MACHINE)
- `MAX_WORDS` (MACHINE)
- `LONG_GREETING` (MACHINE)
- `TOO_LONG` (NOTSURE)

# Usage

Set a Dialplan as follow (see `conf/dialplan.xml`):

```xml
    <extension name="amd_ext" continue="false">
      <condition field="destination_number" expression="^5555$">
        <action application="answer"/>
          <action application="mod_amd"/>
          <action application="playback" data="/usr/local/freeswitch/sounds/en/us/callie/voicemail/8000/vm-hello.wav"/>
          <action application="info"/>
          <action application="hangup"/>
      </condition>
    </extension>
```

The originate a call that will bridge to the `amd_ext` dialplan:

```sh
originate {origination_caller_id_number='808111222',ignore_early_media=true,originate_timeout=45}sofia/gateway/mygateway/0044888888888 5555
```