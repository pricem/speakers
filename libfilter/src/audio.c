#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <portaudio.h>
#include <sndfile.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>


#include "audio.h"
#include "common.h"
#include "circular_buffer.h"
#include "os_filter.h"

#ifdef LINUX_ALSA
#include <pa_linux_alsa.h>
#endif

#define DEBUG_PRINT_INTERVAL_MILLIS (1000)
#define WAV_BUFFER_SIZE (32768)


CircularBuffer * __read_audio_file(AudioOptions audio_options);

static int recordCallback(const void *input_buffer,
                          void *output_buffer,
                          unsigned long frames_per_buffer,
                          const PaStreamCallbackTimeInfo* time_info,
                          PaStreamCallbackFlags status_flags,
                          void *user_data)
{
    (void)output_buffer;
    (void)time_info;
    (void)status_flags;

    CircularBuffer * buffer = (CircularBuffer *)user_data;
    const NUMERIC * reader = (const NUMERIC*)input_buffer;


    CircularBuffer_produce_blocking(buffer, reader, frames_per_buffer * 2);

    return paContinue;
}


typedef struct {
    CircularBuffer * buffer;
    int num_output_channels;
} PlaybackCallbackData;

static int playCallback(const void *input_buffer,
                        void *output_buffer,
                        unsigned long frames_per_buffer,
                        const PaStreamCallbackTimeInfo* time_info,
                        PaStreamCallbackFlags status_flags,
                        void *user_data)
{
    (void)input_buffer;
    (void)time_info;
    (void)status_flags;

    PlaybackCallbackData * callback_data = (PlaybackCallbackData *)user_data;
    CircularBuffer * buffer = callback_data->buffer;
    NUMERIC * writer = (NUMERIC *)output_buffer;
    CircularBuffer_consume_blocking(buffer, writer, frames_per_buffer * callback_data->num_output_channels, 0);
    return paContinue;
}

int is_parent_running(pthread_t parent_tid);

int run_filter(AudioOptions audio_options)
{
    PaStreamParameters input_parameters, output_parameters;
    PaStream *input_stream = NULL, *output_stream = NULL;
    PaError err = paNoError;
    CircularBuffer * output_buffer = CircularBuffer_create(audio_options.buffer_size);
    CircularBuffer * input_buffer = NULL;
    int live_stream = !audio_options.wav_path || !strlen(audio_options.wav_path);

    OSFilter * filter =
        OSFilter_create(
                        audio_options.filters,
                        audio_options.num_filters,
                        2,
                        audio_options.filter_size,
                        audio_options.conv_multiple * (audio_options.filter_size - 1),
                        audio_options.input_scale
                        );
    if (!filter) {
        goto done;
    }

    if ((err = Pa_Initialize()) != paNoError) {
        goto done;
    }

    int step_size = audio_options.filter_size - 1;


    if (live_stream) {
        input_buffer = CircularBuffer_create(audio_options.buffer_size);
        input_parameters.device = audio_options.input_device;
        input_parameters.channelCount = 2;
        input_parameters.sampleFormat = PA_SAMPLE_TYPE;
        input_parameters.suggestedLatency = Pa_GetDeviceInfo(input_parameters.device)->defaultHighInputLatency;
        input_parameters.hostApiSpecificStreamInfo = NULL;

        err = Pa_OpenStream(
                            &input_stream,
                            &input_parameters,
                            NULL,
                            audio_options.sample_rate,
                            step_size,
                            paNoFlag,
                            recordCallback,
                            input_buffer
                            );

        if (err != paNoError) {
            goto done;
        }


#ifdef LINUX_ALSA
        PaAlsa_EnableRealtimeScheduling(input_stream, 1);
#endif

        if ((err = Pa_StartStream(input_stream)) != paNoError) {
            goto done;
        }
    } else {
        input_buffer = __read_audio_file(audio_options);
    }

    output_parameters.device = audio_options.output_device;
    if (audio_options.output_channels >= 6) {
        output_parameters.channelCount = audio_options.output_channels + 2;
    } else {
        output_parameters.channelCount = audio_options.output_channels;
    }
    output_parameters.sampleFormat = PA_SAMPLE_TYPE;
    output_parameters.suggestedLatency = Pa_GetDeviceInfo(output_parameters.device)->defaultLowOutputLatency;
    output_parameters.hostApiSpecificStreamInfo = NULL;

    PlaybackCallbackData playback_data = {
        output_buffer,
        output_parameters.channelCount
    };

    printf("output channels: %d\n", output_parameters.channelCount);

    err = Pa_OpenStream(
                        &output_stream,
                        NULL,
                        &output_parameters,
                        audio_options.sample_rate,
                        step_size,
                        paNoFlag,
                        playCallback,
                        &playback_data
                        );

    if (err != paNoError) {
        goto done;
    }

#ifdef LINUX_ALSA
    PaAlsa_EnableRealtimeScheduling(output_stream, 1);
#endif

    if ((err = Pa_StartStream(output_stream)) != paNoError) {
        goto done;
    }

    int output_scale = output_parameters.channelCount / 2;
    int frame_print_interval = DEBUG_PRINT_INTERVAL_MILLIS * audio_options.sample_rate / 1000;
    int current_frame = 0;

    while (true) {
        if ((err = Pa_IsStreamActive(output_stream)) != 1) {
            break;
        }
        if (input_stream && (err = Pa_IsStreamActive(input_stream)) != 1) {
            break;
        }
        current_frame += OSFilter_execute(filter, input_buffer, output_buffer);
        if (audio_options.print_debug && current_frame > frame_print_interval) {
            current_frame -= frame_print_interval;

            if (!is_parent_running(audio_options.parent_thread_ident)) {
                printf("Parent thread is dead. Shutting down now.\n");
                fflush(stdout);
                goto done;
            }

            if (!audio_options.print_debug) {
                continue;
            }
            long frame_difference = (CircularBuffer_lag(input_buffer) + CircularBuffer_lag(output_buffer) / output_scale) / 2;
            float lag = (float)(frame_difference) / audio_options.sample_rate * 1000;
            printf("%lu\t%lu\t%lu\t%fms\n",
                   input_buffer->offset_producer,
                   output_buffer->offset_consumer / output_scale,
                   frame_difference, lag
                   );

            if (live_stream && (lag > audio_options.lag_reset_limit * 1000)) {
                printf("Resetting to latest due to high lag.\n");
                CircularBuffer_fastforward(input_buffer, audio_options.filter_size * 2);
                CircularBuffer_fastforward(output_buffer, 0);
            }
            fflush(stdout);
        }
    }
    if (err < 0) {
        goto done;
    }

done:
    if (output_stream) {
        Pa_CloseStream(output_stream);
    }
    if (input_stream) {
        Pa_CloseStream(input_stream);
    }
    Pa_Terminate();
    if (input_buffer) {
        CircularBuffer_destroy(input_buffer);
    }
    CircularBuffer_destroy(output_buffer);
    OSFilter_destroy(filter);

    if (err != paNoError) {
        fprintf(stdout, "An error occured while using the portaudio stream\n");
        fprintf(stdout, "Error number: %d\n", err);
        fprintf(stdout, "Error message: %s\n", Pa_GetErrorText(err));
        fflush(stdout);
        err = 1;
    }
    return err;
}

int is_parent_running(pthread_t parent_tid)
{
    return pthread_kill(parent_tid, 0) != ESRCH;
}


CircularBuffer * __read_audio_file(AudioOptions audio_options)
{
    struct stat st;
    SF_INFO sf_info;
    long wav_file_size;

    float * buffer = NULL;
    CircularBuffer * input_buffer = NULL;
    SNDFILE * wav_file = NULL;

    if (stat(audio_options.wav_path, &st) == 0) {
        wav_file_size = st.st_size;
    } else {
        printf("Could not open wav file: %s\n", audio_options.wav_path);
        fflush(stdout);
        goto error;
    }

    input_buffer = CircularBuffer_create(wav_file_size);

    printf("Opening wave file %s with size %ld\n", audio_options.wav_path, wav_file_size);

    if (!(wav_file = sf_open(audio_options.wav_path, SFM_READ, &sf_info))) {
        printf("Could not open wav file: %s\n", audio_options.wav_path);
        fflush(stdout);
        goto error;
    }

    buffer =  (float *)malloc(sizeof(float) * WAV_BUFFER_SIZE);

    if (!buffer) {
        sf_close(wav_file);
        goto error;
    }

    int readcount;
    while ((readcount = sf_read_float(wav_file, buffer, WAV_BUFFER_SIZE))) {
        CircularBuffer_produce_blocking(input_buffer, buffer, readcount);
    }

    free(buffer);
    sf_close(wav_file);
    return input_buffer;

 error:
    if (buffer) {
        free(buffer);
    }
    if (input_buffer) {
        CircularBuffer_destroy(input_buffer);
    }
    if (wav_file) {
        sf_close(wav_file);
    }
    return NULL;
}


#ifdef AUDIO_MAIN

int main(int argc, char ** argv)
{
    (void)argc;
    (void)argv;


    AudioOptions audio_options = {
        48000,
        0,
        0,
        2,
        NULL,
        1,
        3,
        4,
        1024 * 1024,
        1,
        0.5,
        NULL,
        0.10,
        pthread_self()
    };

    audio_options.filters = (NUMERIC *)malloc(sizeof(NUMERIC) * audio_options.num_filters * audio_options.filter_size);
    for (int i = 0; i < audio_options.num_filters * audio_options.filter_size; ++i) {
        audio_options.filters[i] = 1 / ((double) audio_options.filter_size);
    }

    int ret_val = run_filter(audio_options);

    free(audio_options.filters);

    return ret_val;
}


#endif
