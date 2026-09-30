#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sample_venc_lib.h"

#define CROW_VERSION "0.9"
#define CROW_FPS 25
#define CROW_SEGMENT_FRAMES 50
#define CROW_SEGMENT_SECONDS 2

/* 30 completed 2-second segments = one minute of rewind history. */
#define CROW_RING_SEGMENTS 30
#define CROW_RING_SECONDS (CROW_RING_SEGMENTS * CROW_SEGMENT_SECONDS)

/* Default Hindsight tail after the trigger. Retriggering extends this tail. */
#define CROW_POST_SEGMENTS 15
#define CROW_POST_SECONDS (CROW_POST_SEGMENTS * CROW_SEGMENT_SECONDS)

#define CROW_SEGMENT_DIR "/root/crow_segments"
#define CROW_EVENT_DIR "/root/crow_events"
#define CROW_RECORDING_DIR "/root/crow_recordings"
#define CROW_FIRST_OUTPUT "/root/crow_segments/segment_000000"

#define CROW_AUDIO_DIR "/root/crow_audio"
#define CROW_AUDIO_PID_FILE "/root/crow_audio.pid"
#define CROW_STATUS_FILE "/root/crow_status.json"
#define CROW_AUDIO_RATE 48000
#define CROW_AUDIO_CHANNELS 1
#define CROW_AUDIO_SEGMENT_SECONDS 2
/* Keep 90 sec of microphone history so the 60 sec Hindsight window has margin. */
#define CROW_AUDIO_RING_SEGMENTS 45

struct encoder_job {
    int argc;
    char **argv;
    int result;
    int done;
    pthread_mutex_t lock;
};

struct segment_scan {
    int have_any;
    uint32_t min_index;
    uint32_t max_index;
    int count;
};

struct audio_scan {
    int have_any;
    uint32_t min_index;
    uint32_t max_index;
    int count;
};

static pid_t audio_pid = -1;

struct event_state {
    int pending;
    int active;
    int event_id;
    uint32_t trigger_index;
    uint32_t next_post_index;
    uint32_t post_goal_index;
    int pre_saved;
    int post_saved;
    uint32_t audio_trigger_index;
    uint32_t next_audio_index;
    int audio_pre_saved;
    int audio_post_saved;
    char event_path[PATH_MAX];
};

struct recording_state {
    int active;
    int stop_pending;
    int recording_id;
    uint32_t start_index;
    uint32_t next_index;
    uint32_t stop_index;
    int segments_saved;
    uint32_t audio_start_index;
    uint32_t next_audio_index;
    uint32_t audio_stop_index;
    int audio_segments_saved;
    char recording_path[PATH_MAX];
};

static void *encoder_thread_main(void *opaque)
{
    struct encoder_job *job = (struct encoder_job *)opaque;
    int result = venc_main(job->argc, job->argv);

    pthread_mutex_lock(&job->lock);
    job->result = result;
    job->done = 1;
    pthread_mutex_unlock(&job->lock);

    return NULL;
}

static int encoder_job_done(struct encoder_job *job)
{
    int done;

    pthread_mutex_lock(&job->lock);
    done = job->done;
    pthread_mutex_unlock(&job->lock);

    return done;
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void *encoder_stop_wake_thread(void *opaque)
{
    (void)opaque;
    crow_wake_encoder_stop();
    return NULL;
}

static int parse_segment_index(const char *name, uint32_t *index_out)
{
    const char *prefix = "segment_";
    const char *suffix = ".h265";
    size_t len;
    size_t suffix_len = strlen(suffix);
    char number[32];
    char *end = NULL;
    unsigned long value;
    size_t number_len;

    if (strncmp(name, prefix, strlen(prefix)) != 0)
        return 0;

    len = strlen(name);
    if (len <= strlen(prefix) + suffix_len)
        return 0;

    if (strcmp(name + len - suffix_len, suffix) != 0)
        return 0;

    number_len = len - strlen(prefix) - suffix_len;
    if (number_len == 0 || number_len >= sizeof(number))
        return 0;

    memcpy(number, name + strlen(prefix), number_len);
    number[number_len] = '\0';

    errno = 0;
    value = strtoul(number, &end, 10);
    if (errno != 0 || end == number || *end != '\0' || value > UINT32_MAX)
        return 0;

    *index_out = (uint32_t)value;
    return 1;
}

static void segment_path(uint32_t index, char *path, size_t path_size)
{
    snprintf(path, path_size, "%s/segment_%06u.h265", CROW_SEGMENT_DIR, index);
}

static int scan_segments(struct segment_scan *scan)
{
    DIR *dir;
    struct dirent *entry;
    uint32_t index;

    memset(scan, 0, sizeof(*scan));

    dir = opendir(CROW_SEGMENT_DIR);
    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (!parse_segment_index(entry->d_name, &index))
            continue;

        if (!scan->have_any) {
            scan->have_any = 1;
            scan->min_index = index;
            scan->max_index = index;
        } else {
            if (index < scan->min_index)
                scan->min_index = index;
            if (index > scan->max_index)
                scan->max_index = index;
        }

        scan->count++;
    }

    closedir(dir);
    return 0;
}

static int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Could not create %s: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
}

static int prepare_segment_dir(void)
{
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];
    uint32_t ignored;

    if (ensure_dir(CROW_SEGMENT_DIR) != 0)
        return -1;

    dir = opendir(CROW_SEGMENT_DIR);
    if (!dir) {
        fprintf(stderr, "Could not open %s: %s\n",
                CROW_SEGMENT_DIR, strerror(errno));
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!parse_segment_index(entry->d_name, &ignored))
            continue;

        snprintf(path, sizeof(path), "%s/%s", CROW_SEGMENT_DIR, entry->d_name);
        if (unlink(path) != 0) {
            fprintf(stderr, "Could not remove old ring segment %s: %s\n",
                    path, strerror(errno));
            closedir(dir);
            return -1;
        }
    }

    closedir(dir);
    return 0;
}

static int next_numbered_dir_id(const char *root, const char *prefix)
{
    DIR *dir;
    struct dirent *entry;
    int highest = 0;
    int id;
    char extra;
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "%s%%d%%c", prefix);

    dir = opendir(root);
    if (!dir)
        return 1;

    while ((entry = readdir(dir)) != NULL) {
        extra = '\0';
        if (sscanf(entry->d_name, pattern, &id, &extra) == 1 && id > highest)
            highest = id;
    }

    closedir(dir);
    return highest + 1;
}

static int copy_stream(FILE *in, FILE *out)
{
    unsigned char buffer[64 * 1024];
    size_t bytes;

    while ((bytes = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, bytes, out) != bytes)
            return -1;
    }

    if (ferror(in))
        return -1;

    return 0;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = NULL;
    FILE *out = NULL;
    int result = -1;

    in = fopen(src, "rb");
    if (!in)
        goto done;

    out = fopen(dst, "wb");
    if (!out)
        goto done;

    if (copy_stream(in, out) != 0)
        goto done;

    if (fflush(out) != 0)
        goto done;

    result = 0;

done:
    if (out)
        fclose(out);
    if (in)
        fclose(in);

    if (result != 0)
        unlink(dst);

    return result;
}

/*
 * Event/recording parts normally use hard links. That makes preservation
 * essentially instant and avoids duplicating bytes while the source segment
 * is still present in the rolling ring. If hard links are unavailable, fall
 * back to an ordinary copy.
 */
static int preserve_named_segment(uint32_t source_index,
                                  const char *destination_path,
                                  const char *filename)
{
    char src[PATH_MAX];
    char dst[PATH_MAX];

    segment_path(source_index, src, sizeof(src));
    snprintf(dst, sizeof(dst), "%s/%s", destination_path, filename);

    unlink(dst);

    if (link(src, dst) == 0)
        return 0;

    if (errno == ENOENT)
        return -1;

    fprintf(stderr,
            "Crow: hard link failed for segment %06u (%s); copying instead.\n",
            source_index, strerror(errno));

    return copy_file(src, dst);
}

static int preserve_event_segment(uint32_t source_index,
                                  const char *event_path,
                                  const char *phase,
                                  int ordinal)
{
    char filename[64];

    snprintf(filename, sizeof(filename), "%s_%03d.h265", phase, ordinal);
    return preserve_named_segment(source_index, event_path, filename);
}

static int preserve_recording_segment(uint32_t source_index,
                                      const char *recording_path,
                                      int ordinal)
{
    char filename[64];

    snprintf(filename, sizeof(filename), "part_%06d.h265", ordinal);
    return preserve_named_segment(source_index, recording_path, filename);
}


static int parse_audio_index(const char *name, uint32_t *index_out)
{
    const char *prefix = "audio_";
    const char *suffix = ".pcm";
    size_t len;
    size_t suffix_len = strlen(suffix);
    char number[32];
    char *end = NULL;
    unsigned long value;
    size_t number_len;

    if (strncmp(name, prefix, strlen(prefix)) != 0)
        return 0;

    len = strlen(name);
    if (len <= strlen(prefix) + suffix_len)
        return 0;
    if (strcmp(name + len - suffix_len, suffix) != 0)
        return 0;

    number_len = len - strlen(prefix) - suffix_len;
    if (number_len == 0 || number_len >= sizeof(number))
        return 0;

    memcpy(number, name + strlen(prefix), number_len);
    number[number_len] = '\0';

    errno = 0;
    value = strtoul(number, &end, 10);
    if (errno != 0 || end == number || *end != '\0' || value > UINT32_MAX)
        return 0;

    *index_out = (uint32_t)value;
    return 1;
}

static void audio_segment_path(uint32_t index, char *path, size_t path_size)
{
    snprintf(path, path_size, "%s/audio_%u.pcm", CROW_AUDIO_DIR, index);
}

static int scan_audio(struct audio_scan *scan)
{
    DIR *dir;
    struct dirent *entry;
    uint32_t index;

    memset(scan, 0, sizeof(*scan));
    dir = opendir(CROW_AUDIO_DIR);
    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (!parse_audio_index(entry->d_name, &index))
            continue;
        if (!scan->have_any) {
            scan->have_any = 1;
            scan->min_index = index;
            scan->max_index = index;
        } else {
            if (index < scan->min_index)
                scan->min_index = index;
            if (index > scan->max_index)
                scan->max_index = index;
        }
        scan->count++;
    }

    closedir(dir);
    return 0;
}

static int prepare_audio_dir(void)
{
    DIR *dir;
    struct dirent *entry;
    char path[PATH_MAX];
    uint32_t ignored;

    if (ensure_dir(CROW_AUDIO_DIR) != 0)
        return -1;

    dir = opendir(CROW_AUDIO_DIR);
    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (!parse_audio_index(entry->d_name, &ignored))
            continue;
        snprintf(path, sizeof(path), "%s/%s", CROW_AUDIO_DIR, entry->d_name);
        unlink(path);
    }

    closedir(dir);
    return 0;
}

static int audio_is_running(void)
{
    int status;
    pid_t result;

    if (audio_pid <= 0)
        return 0;

    result = waitpid(audio_pid, &status, WNOHANG);
    if (result == 0)
        return 1;

    audio_pid = -1;
    unlink(CROW_AUDIO_PID_FILE);
    return 0;
}

static int start_audio_capture(void)
{
    pid_t pid;

    if (prepare_audio_dir() != 0) {
        fprintf(stderr, "Crow audio: could not prepare %s. Video will continue without audio.\n",
                CROW_AUDIO_DIR);
        return -1;
    }

    /* Sipeed recommends ADC capture volume 24 for the onboard analog mic. */
    (void)system("amixer -Dhw:0 cset name='ADC Capture Volume' 24 >/dev/null 2>&1");

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Crow audio: fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        int log_fd = open("/root/crow_audio.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            if (log_fd > STDERR_FILENO)
                close(log_fd);
        }

        execlp("arecord", "arecord",
               "-Dhw:0,0",
               "-r", "48000",
               "-f", "S16_LE",
               "-c", "1",
               "-t", "raw",
               "--max-file-time", "2",
               "--use-strftime",
               CROW_AUDIO_DIR "/audio_%v.pcm",
               (char *)NULL);
        _exit(127);
    }

    audio_pid = pid;
    {
        FILE *pid_file = fopen(CROW_AUDIO_PID_FILE, "w");
        if (pid_file) {
            fprintf(pid_file, "%d\n", (int)audio_pid);
            fclose(pid_file);
        }
    }
    usleep(300000);

    if (!audio_is_running()) {
        fprintf(stderr,
                "Crow audio: arecord exited during startup. Video will continue without audio. See /root/crow_audio.log\n");
        return -1;
    }

    printf("Onboard microphone capture active: 48 kHz mono S16_LE, 2-sec PCM segments (PID %d).\n",
           (int)audio_pid);
    return 0;
}

/*
 * Force arecord to close the current PCM file and immediately open the next.
 * arecord documents SIGUSR1 as a file-rotation request.  Returning the just-
 * closed index gives Crow a clean audio boundary for record/Hindsight actions.
 */
static int64_t rotate_audio_boundary(void)
{
    struct audio_scan before;
    struct audio_scan after;
    int tries;

    if (!audio_is_running())
        return -1;

    memset(&before, 0, sizeof(before));
    (void)scan_audio(&before);

    if (kill(audio_pid, SIGUSR1) != 0)
        return -1;

    for (tries = 0; tries < 10; tries++) {
        usleep(50000);
        memset(&after, 0, sizeof(after));
        if (scan_audio(&after) == 0 && after.have_any) {
            if (!before.have_any || after.max_index > before.max_index)
                return (int64_t)after.max_index - 1;
        }
    }

    if (before.have_any)
        return (int64_t)before.max_index;
    return -1;
}

static void stop_audio_capture(void)
{
    int status;
    int tries;

    if (!audio_is_running())
        return;

    kill(audio_pid, SIGTERM);
    for (tries = 0; tries < 20; tries++) {
        pid_t result = waitpid(audio_pid, &status, WNOHANG);
        if (result == audio_pid) {
            audio_pid = -1;
            unlink(CROW_AUDIO_PID_FILE);
            return;
        }
        usleep(50000);
    }

    kill(audio_pid, SIGKILL);
    waitpid(audio_pid, &status, 0);
    audio_pid = -1;
    unlink(CROW_AUDIO_PID_FILE);
}

static int preserve_named_audio(uint32_t source_index,
                                const char *destination_path,
                                const char *filename)
{
    char src[PATH_MAX];
    char dst[PATH_MAX];

    audio_segment_path(source_index, src, sizeof(src));
    snprintf(dst, sizeof(dst), "%s/%s", destination_path, filename);
    unlink(dst);

    if (link(src, dst) == 0)
        return 0;
    if (errno == ENOENT)
        return -1;
    return copy_file(src, dst);
}

static int preserve_event_audio(uint32_t source_index,
                                const char *event_path,
                                const char *phase,
                                int ordinal)
{
    char filename[64];
    snprintf(filename, sizeof(filename), "audio_%s_%03d.pcm", phase, ordinal);
    return preserve_named_audio(source_index, event_path, filename);
}

static int preserve_recording_audio(uint32_t source_index,
                                    const char *recording_path,
                                    int ordinal)
{
    char filename[64];
    snprintf(filename, sizeof(filename), "audio_%06d.pcm", ordinal);
    return preserve_named_audio(source_index, recording_path, filename);
}

static void prune_audio_ring(void)
{
    struct audio_scan scan;
    DIR *dir;
    struct dirent *entry;
    uint32_t index;
    uint32_t completed_max;
    uint32_t oldest_keep;
    char path[PATH_MAX];

    if (scan_audio(&scan) != 0 || !scan.have_any)
        return;

    completed_max = audio_is_running() && scan.max_index > 0
        ? scan.max_index - 1
        : scan.max_index;

    if (completed_max < (CROW_AUDIO_RING_SEGMENTS - 1))
        return;

    oldest_keep = completed_max - (CROW_AUDIO_RING_SEGMENTS - 1);
    dir = opendir(CROW_AUDIO_DIR);
    if (!dir)
        return;

    while ((entry = readdir(dir)) != NULL) {
        if (!parse_audio_index(entry->d_name, &index))
            continue;
        if (index >= oldest_keep)
            continue;
        snprintf(path, sizeof(path), "%s/%s", CROW_AUDIO_DIR, entry->d_name);
        unlink(path);
    }
    closedir(dir);
}

static void append_text_file(const char *dir, const char *name, const char *format, ...)
{
    char path[PATH_MAX];
    FILE *file;
    va_list args;

    if (!dir || dir[0] == '\0')
        return;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    file = fopen(path, "a");
    if (!file)
        return;

    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fclose(file);
}

static int append_file_to_output(FILE *out, const char *path)
{
    FILE *in = fopen(path, "rb");
    int result;

    if (!in)
        return -1;

    result = copy_stream(in, out);
    fclose(in);
    return result;
}

static int finalize_assembled_file(FILE *out,
                                  const char *temporary_path,
                                  const char *final_path)
{
    int fd;
    int result = 0;

    if (fflush(out) != 0)
        result = -1;

    fd = fileno(out);
    if (result == 0 && fd >= 0 && fsync(fd) != 0)
        result = -1;

    if (fclose(out) != 0)
        result = -1;

    if (result != 0)
        return -1;

    /*
     * The web UI only sees the final .h265 name.  Keep assembly hidden
     * behind .part, then atomically publish it after every byte is closed
     * and flushed.  This prevents downloads of a file that is still growing.
     */
    if (rename(temporary_path, final_path) != 0)
        return -1;

    return 0;
}

static int assemble_event(struct event_state *event)
{
    char output_path[PATH_MAX];
    char temporary_path[PATH_MAX];
    char input_path[PATH_MAX];
    FILE *out;
    int i;

    snprintf(output_path, sizeof(output_path),
             "%s/event_%04d.h265", event->event_path, event->event_id);
    snprintf(temporary_path, sizeof(temporary_path),
             "%s/event_%04d.h265.part", event->event_path, event->event_id);

    unlink(temporary_path);
    out = fopen(temporary_path, "wb");
    if (!out)
        return -1;

    for (i = 0; i < event->pre_saved; i++) {
        snprintf(input_path, sizeof(input_path),
                 "%s/pre_%03d.h265", event->event_path, i);
        if (append_file_to_output(out, input_path) != 0)
            goto fail;
    }

    for (i = 0; i < event->post_saved; i++) {
        snprintf(input_path, sizeof(input_path),
                 "%s/post_%03d.h265", event->event_path, i);
        if (append_file_to_output(out, input_path) != 0)
            goto fail;
    }

    if (finalize_assembled_file(out, temporary_path, output_path) != 0)
        goto fail_closed;

    return 0;

fail:
    fclose(out);
fail_closed:
    unlink(temporary_path);
    return -1;
}

static int assemble_recording(struct recording_state *recording)
{
    char output_path[PATH_MAX];
    char temporary_path[PATH_MAX];
    char input_path[PATH_MAX];
    FILE *out;
    int i;

    snprintf(output_path, sizeof(output_path),
             "%s/recording_%04d.h265",
             recording->recording_path, recording->recording_id);
    snprintf(temporary_path, sizeof(temporary_path),
             "%s/recording_%04d.h265.part",
             recording->recording_path, recording->recording_id);

    unlink(temporary_path);
    out = fopen(temporary_path, "wb");
    if (!out)
        return -1;

    for (i = 0; i < recording->segments_saved; i++) {
        snprintf(input_path, sizeof(input_path),
                 "%s/part_%06d.h265", recording->recording_path, i);
        if (append_file_to_output(out, input_path) != 0)
            goto fail;
    }

    if (finalize_assembled_file(out, temporary_path, output_path) != 0)
        goto fail_closed;

    return 0;

fail:
    fclose(out);
fail_closed:
    unlink(temporary_path);
    return -1;
}


static int assemble_event_audio(struct event_state *event)
{
    char output_path[PATH_MAX];
    char temporary_path[PATH_MAX];
    char input_path[PATH_MAX];
    FILE *out;
    int i;

    if (event->audio_pre_saved + event->audio_post_saved <= 0)
        return 1;

    snprintf(output_path, sizeof(output_path),
             "%s/event_%04d.pcm", event->event_path, event->event_id);
    snprintf(temporary_path, sizeof(temporary_path),
             "%s/event_%04d.pcm.part", event->event_path, event->event_id);

    unlink(temporary_path);
    out = fopen(temporary_path, "wb");
    if (!out)
        return -1;

    for (i = 0; i < event->audio_pre_saved; i++) {
        snprintf(input_path, sizeof(input_path),
                 "%s/audio_pre_%03d.pcm", event->event_path, i);
        if (append_file_to_output(out, input_path) != 0)
            goto fail;
    }
    for (i = 0; i < event->audio_post_saved; i++) {
        snprintf(input_path, sizeof(input_path),
                 "%s/audio_post_%03d.pcm", event->event_path, i);
        if (append_file_to_output(out, input_path) != 0)
            goto fail;
    }

    if (finalize_assembled_file(out, temporary_path, output_path) != 0)
        goto fail_closed;
    return 0;

fail:
    fclose(out);
fail_closed:
    unlink(temporary_path);
    return -1;
}

static int assemble_recording_audio(struct recording_state *recording)
{
    char output_path[PATH_MAX];
    char temporary_path[PATH_MAX];
    char input_path[PATH_MAX];
    FILE *out;
    int i;

    if (recording->audio_segments_saved <= 0)
        return 1;

    snprintf(output_path, sizeof(output_path),
             "%s/recording_%04d.pcm",
             recording->recording_path, recording->recording_id);
    snprintf(temporary_path, sizeof(temporary_path),
             "%s/recording_%04d.pcm.part",
             recording->recording_path, recording->recording_id);

    unlink(temporary_path);
    out = fopen(temporary_path, "wb");
    if (!out)
        return -1;

    for (i = 0; i < recording->audio_segments_saved; i++) {
        snprintf(input_path, sizeof(input_path),
                 "%s/audio_%06d.pcm", recording->recording_path, i);
        if (append_file_to_output(out, input_path) != 0)
            goto fail;
    }

    if (finalize_assembled_file(out, temporary_path, output_path) != 0)
        goto fail_closed;
    return 0;

fail:
    fclose(out);
fail_closed:
    unlink(temporary_path);
    return -1;
}

static void service_event_audio(struct event_state *event)
{
    struct audio_scan scan;
    int64_t completed_max;

    if (!event->active || event->next_audio_index == UINT32_MAX)
        return;
    if (scan_audio(&scan) != 0 || !scan.have_any)
        return;

    completed_max = audio_is_running()
        ? ((int64_t)scan.max_index - 1)
        : (int64_t)scan.max_index;

    while (event->next_audio_index <= (uint32_t)completed_max) {
        uint32_t index = event->next_audio_index;
        int ordinal = event->audio_post_saved;
        if (preserve_event_audio(index, event->event_path, "post", ordinal) == 0) {
            event->audio_post_saved++;
        }
        event->next_audio_index++;
    }
}

static void service_recording_audio(struct recording_state *recording)
{
    struct audio_scan scan;
    int64_t completed_max;

    if (!recording->active || recording->next_audio_index == UINT32_MAX)
        return;
    if (scan_audio(&scan) != 0 || !scan.have_any)
        return;

    completed_max = audio_is_running()
        ? ((int64_t)scan.max_index - 1)
        : (int64_t)scan.max_index;

    while (recording->next_audio_index <= (uint32_t)completed_max) {
        uint32_t index = recording->next_audio_index;
        int ordinal = recording->audio_segments_saved;

        if (recording->audio_stop_index != UINT32_MAX &&
            index > recording->audio_stop_index)
            break;

        if (preserve_recording_audio(index, recording->recording_path, ordinal) == 0)
            recording->audio_segments_saved++;
        recording->next_audio_index++;
    }
}

static int start_event(struct event_state *event)
{
    uint32_t first;
    uint32_t index;
    int ordinal = 0;
    int id;

    id = next_numbered_dir_id(CROW_EVENT_DIR, "event_");
    snprintf(event->event_path, sizeof(event->event_path),
             "%s/event_%04d", CROW_EVENT_DIR, id);

    if (mkdir(event->event_path, 0755) != 0) {
        fprintf(stderr, "Could not create event directory %s: %s\n",
                event->event_path, strerror(errno));
        event->event_path[0] = '\0';
        return -1;
    }

    event->event_id = id;
    event->pre_saved = 0;
    event->post_saved = 0;
    event->audio_pre_saved = 0;
    event->audio_post_saved = 0;
    event->next_audio_index = UINT32_MAX;

    if (event->trigger_index >= (CROW_RING_SEGMENTS - 1))
        first = event->trigger_index - (CROW_RING_SEGMENTS - 1);
    else
        first = 0;

    append_text_file(event->event_path, "manifest.txt",
                     "Crow Hindsight event %04d\n"
                     "crow_version=%s\n"
                     "segment_seconds=%d\n"
                     "requested_pre_seconds=%d\n"
                     "default_post_seconds=%d\n"
                     "trigger_segment=%06u\n",
                     id,
                     CROW_VERSION,
                     CROW_SEGMENT_SECONDS,
                     CROW_RING_SECONDS,
                     CROW_POST_SECONDS,
                     event->trigger_index);

    for (index = first; index <= event->trigger_index; index++) {
        if (preserve_event_segment(index, event->event_path, "pre", ordinal) == 0) {
            append_text_file(event->event_path, "manifest.txt",
                             "pre_%03d.h265 <- segment_%06u.h265\n",
                             ordinal, index);
            ordinal++;
        }
    }

    event->pre_saved = ordinal;

    if (event->audio_trigger_index != UINT32_MAX) {
        uint32_t audio_first = event->audio_trigger_index >= (CROW_RING_SEGMENTS - 1)
            ? event->audio_trigger_index - (CROW_RING_SEGMENTS - 1)
            : 0;
        uint32_t audio_index;
        int audio_ordinal = 0;

        for (audio_index = audio_first;
             audio_index <= event->audio_trigger_index;
             audio_index++) {
            if (preserve_event_audio(audio_index, event->event_path,
                                     "pre", audio_ordinal) == 0) {
                append_text_file(event->event_path, "manifest.txt",
                                 "audio_pre_%03d.pcm <- audio_%u.pcm\n",
                                 audio_ordinal, audio_index);
                audio_ordinal++;
            }
        }
        event->audio_pre_saved = audio_ordinal;
        event->next_audio_index = event->audio_trigger_index + 1;
        append_text_file(event->event_path, "manifest.txt",
                         "audio_pre_segments_saved=%d\n",
                         event->audio_pre_saved);
    }

    event->next_post_index = event->trigger_index + 1;
    event->post_goal_index = event->trigger_index + CROW_POST_SEGMENTS;
    event->active = 1;
    event->pending = 0;

    append_text_file(event->event_path, "manifest.txt",
                     "pre_segments_saved=%d\n", event->pre_saved);
    append_text_file(event->event_path, "manifest.txt", "state=capturing_post\n");

    printf("\nHindsight event %04d created.\n", event->event_id);
    printf("Saved %d pre-event segment%s (~%d sec available).\n",
           event->pre_saved,
           event->pre_saved == 1 ? "" : "s",
           event->pre_saved * CROW_SEGMENT_SECONDS);
    printf("Capturing at least ~%d seconds after the trigger.\n", CROW_POST_SECONDS);
    printf("Retrigger Hindsight to extend the event another ~%d sec from the new trigger.\n",
           CROW_POST_SECONDS);
    printf("Event directory: %s\n", event->event_path);

    return 0;
}

static void finish_event(struct event_state *event, const char *state)
{
    int assembled;
    int audio_assembled;

    if (event->next_audio_index != UINT32_MAX) {
        (void)rotate_audio_boundary();
        service_event_audio(event);
    }

    append_text_file(event->event_path, "manifest.txt",
                     "post_segments_saved=%d\n", event->post_saved);
    append_text_file(event->event_path, "manifest.txt", "state=%s\n", state);

    assembled = assemble_event(event);
    audio_assembled = assemble_event_audio(event);
    append_text_file(event->event_path, "manifest.txt",
                     "assembled_h265=%s\n", assembled == 0 ? "yes" : "no");
    append_text_file(event->event_path, "manifest.txt",
                     "audio_segments_saved=%d\nassembled_pcm=%s\n",
                     event->audio_pre_saved + event->audio_post_saved,
                     audio_assembled == 0 ? "yes" : (audio_assembled == 1 ? "none" : "no"));

    printf("Hindsight event %04d %s: %d pre + %d post segments.\n",
           event->event_id,
           strcmp(state, "complete") == 0 ? "complete" : "closed",
           event->pre_saved,
           event->post_saved);

    if (assembled == 0)
        printf("Assembled event_%04d.h265 successfully.\n", event->event_id);
    else
        fprintf(stderr, "Crow: could not assemble event_%04d.h265. Parts are preserved.\n",
                event->event_id);

    if (audio_assembled == 0)
        printf("Assembled event_%04d.pcm microphone audio (%d segments).\n",
               event->event_id, event->audio_pre_saved + event->audio_post_saved);
    else if (audio_assembled < 0)
        fprintf(stderr, "Crow: could not assemble event_%04d.pcm. Audio parts are preserved.\n",
                event->event_id);

    printf("Saved at %s\n", event->event_path);

    event->active = 0;
}

static void service_event(struct event_state *event, int64_t completed_max)
{
    if (completed_max < 0)
        return;

    if (event->pending && completed_max >= (int64_t)event->trigger_index) {
        if (start_event(event) != 0) {
            event->pending = 0;
            event->active = 0;
        }
    }

    while (event->active &&
           event->next_post_index <= event->post_goal_index &&
           event->next_post_index <= (uint32_t)completed_max) {
        int ordinal = event->post_saved;
        uint32_t source_index = event->next_post_index;

        if (preserve_event_segment(source_index,
                                   event->event_path,
                                   "post",
                                   ordinal) == 0) {
            append_text_file(event->event_path, "manifest.txt",
                             "post_%03d.h265 <- segment_%06u.h265\n",
                             ordinal,
                             source_index);
            event->post_saved++;
        } else {
            fprintf(stderr,
                    "Crow: could not preserve post segment %06u.\n",
                    source_index);
        }

        event->next_post_index++;
    }

    if (event->active &&
        event->next_post_index > event->post_goal_index &&
        completed_max >= (int64_t)event->post_goal_index) {
        finish_event(event, "complete");
    }
}

static void request_hindsight(struct event_state *event)
{
    struct segment_scan scan;
    uint32_t new_goal;

    if (scan_segments(&scan) != 0 || !scan.have_any) {
        printf("Hindsight request received before the first segment exists. Try again shortly.\n");
        return;
    }

    if (event->active) {
        new_goal = scan.max_index + CROW_POST_SEGMENTS;
        if (new_goal > event->post_goal_index) {
            event->post_goal_index = new_goal;
            append_text_file(event->event_path, "manifest.txt",
                             "retrigger_segment=%06u new_post_goal=%06u\n",
                             scan.max_index,
                             event->post_goal_index);
            printf("Hindsight event %04d extended: capture continues ~%d sec after this trigger.\n",
                   event->event_id, CROW_POST_SECONDS);
        }
        return;
    }

    if (event->pending) {
        int64_t audio_boundary = rotate_audio_boundary();
        event->trigger_index = scan.max_index;
        event->audio_trigger_index = audio_boundary >= 0
            ? (uint32_t)audio_boundary : UINT32_MAX;
        printf("Hindsight pending trigger moved to segment %06u.\n",
               event->trigger_index);
        return;
    }

    /*
     * The highest-numbered file is the current open segment. Wait until it
     * closes, then use it as the final pre-event segment. This includes the
     * actual trigger moment with at most one segment of post-trigger overlap.
     */
    {
        int64_t audio_boundary = rotate_audio_boundary();
        event->audio_trigger_index = audio_boundary >= 0
            ? (uint32_t)audio_boundary : UINT32_MAX;
    }
    event->trigger_index = scan.max_index;
    event->pending = 1;
    event->event_id = 0;
    event->event_path[0] = '\0';

    printf("\nHindsight requested on segment %06u.\n", event->trigger_index);
    printf("Preserving the previous ~%d seconds when this segment closes...\n",
           CROW_RING_SECONDS);
}

static void finish_recording(struct recording_state *recording, const char *state)
{
    int assembled;
    int audio_assembled;

    if (recording->next_audio_index != UINT32_MAX && audio_is_running()) {
        int64_t audio_boundary = rotate_audio_boundary();
        if (audio_boundary >= 0)
            recording->audio_stop_index = (uint32_t)audio_boundary;
    }
    service_recording_audio(recording);

    append_text_file(recording->recording_path, "manifest.txt",
                     "segments_saved=%d\n", recording->segments_saved);
    append_text_file(recording->recording_path, "manifest.txt", "state=%s\n", state);

    assembled = assemble_recording(recording);
    audio_assembled = assemble_recording_audio(recording);
    append_text_file(recording->recording_path, "manifest.txt",
                     "assembled_h265=%s\n", assembled == 0 ? "yes" : "no");
    append_text_file(recording->recording_path, "manifest.txt",
                     "audio_segments_saved=%d\nassembled_pcm=%s\n",
                     recording->audio_segments_saved,
                     audio_assembled == 0 ? "yes" : (audio_assembled == 1 ? "none" : "no"));

    printf("Normal recording %04d %s with %d segment%s (~%d sec).\n",
           recording->recording_id,
           strcmp(state, "complete") == 0 ? "complete" : "closed",
           recording->segments_saved,
           recording->segments_saved == 1 ? "" : "s",
           recording->segments_saved * CROW_SEGMENT_SECONDS);

    if (assembled == 0)
        printf("Assembled recording_%04d.h265 successfully.\n", recording->recording_id);
    else
        fprintf(stderr,
                "Crow: could not assemble recording_%04d.h265. Parts are preserved.\n",
                recording->recording_id);

    if (audio_assembled == 0)
        printf("Assembled recording_%04d.pcm microphone audio (%d segments).\n",
               recording->recording_id, recording->audio_segments_saved);
    else if (audio_assembled < 0)
        fprintf(stderr,
                "Crow: could not assemble recording_%04d.pcm. Audio parts are preserved.\n",
                recording->recording_id);

    printf("Saved at %s\n", recording->recording_path);

    recording->active = 0;
    recording->stop_pending = 0;
}

static int start_recording(struct recording_state *recording)
{
    struct segment_scan scan;
    int id;

    if (scan_segments(&scan) != 0 || !scan.have_any) {
        printf("Normal recording requested before the first segment exists. Try again shortly.\n");
        return -1;
    }

    id = next_numbered_dir_id(CROW_RECORDING_DIR, "recording_");
    snprintf(recording->recording_path, sizeof(recording->recording_path),
             "%s/recording_%04d", CROW_RECORDING_DIR, id);

    if (mkdir(recording->recording_path, 0755) != 0) {
        fprintf(stderr, "Could not create recording directory %s: %s\n",
                recording->recording_path, strerror(errno));
        recording->recording_path[0] = '\0';
        return -1;
    }

    recording->recording_id = id;
    recording->start_index = scan.max_index;
    recording->next_index = scan.max_index;
    recording->stop_index = UINT32_MAX;
    recording->segments_saved = 0;
    recording->audio_segments_saved = 0;
    recording->audio_stop_index = UINT32_MAX;
    {
        int64_t audio_boundary = rotate_audio_boundary();
        if (audio_boundary >= 0) {
            /* Keep the audio segment that ended at the command so start timing
             * mirrors the current video segment, which may also contain up to
             * ~2 seconds before the button/command boundary. */
            recording->audio_start_index = (uint32_t)audio_boundary;
            recording->next_audio_index = recording->audio_start_index;
        } else {
            recording->audio_start_index = UINT32_MAX;
            recording->next_audio_index = UINT32_MAX;
        }
    }
    recording->stop_pending = 0;
    recording->active = 1;

    append_text_file(recording->recording_path, "manifest.txt",
                     "Crow normal recording %04d\n"
                     "crow_version=%s\n"
                     "segment_seconds=%d\n"
                     "start_segment=%06u\n"
                     "audio_rate=%d\n"
                     "audio_start_index=%s\n"
                     "state=recording\n",
                     id,
                     CROW_VERSION,
                     CROW_SEGMENT_SECONDS,
                     recording->start_index,
                     CROW_AUDIO_RATE,
                     recording->audio_start_index == UINT32_MAX ? "none" : "set");

    printf("\nNormal recording %04d started on segment %06u.\n",
           recording->recording_id, recording->start_index);
    printf("Send the record command again to stop and assemble it.\n");
    printf("Recording directory: %s\n", recording->recording_path);

    return 0;
}

static void request_record_toggle(struct recording_state *recording)
{
    struct segment_scan scan;

    if (!recording->active) {
        start_recording(recording);
        return;
    }

    if (recording->stop_pending) {
        printf("Normal recording %04d is already stopping; waiting for segment %06u to close.\n",
               recording->recording_id, recording->stop_index);
        return;
    }

    if (scan_segments(&scan) != 0 || !scan.have_any) {
        printf("Could not determine current segment for recording stop.\n");
        return;
    }

    recording->stop_index = scan.max_index;
    /* Keep microphone capture running until the video stop segment closes.
     * finish_recording() rotates audio at that point for a tighter A/V tail. */
    recording->audio_stop_index = UINT32_MAX;
    recording->stop_pending = 1;
    append_text_file(recording->recording_path, "manifest.txt",
                     "stop_requested_segment=%06u\n",
                     recording->stop_index);

    printf("Normal recording %04d stop requested on segment %06u.\n",
           recording->recording_id, recording->stop_index);
    printf("Finalizing when the current segment closes...\n");
}

static void service_recording(struct recording_state *recording, int64_t completed_max)
{
    if (!recording->active || completed_max < 0)
        return;

    while (recording->next_index <= (uint32_t)completed_max) {
        uint32_t source_index = recording->next_index;
        int ordinal = recording->segments_saved;

        if (recording->stop_pending && source_index > recording->stop_index)
            break;

        if (preserve_recording_segment(source_index,
                                       recording->recording_path,
                                       ordinal) == 0) {
            append_text_file(recording->recording_path, "manifest.txt",
                             "part_%06d.h265 <- segment_%06u.h265\n",
                             ordinal,
                             source_index);
            recording->segments_saved++;
        } else {
            fprintf(stderr,
                    "Crow: could not preserve normal recording segment %06u.\n",
                    source_index);
        }

        recording->next_index++;
    }

    if (recording->stop_pending &&
        completed_max >= (int64_t)recording->stop_index &&
        recording->next_index > recording->stop_index) {
        finish_recording(recording, "complete");
    }
}

static void prune_ring(int64_t completed_max)
{
    DIR *dir;
    struct dirent *entry;
    uint32_t index;
    uint32_t oldest_keep;
    char path[PATH_MAX];

    if (completed_max < (CROW_RING_SEGMENTS - 1))
        return;

    oldest_keep = (uint32_t)completed_max - (CROW_RING_SEGMENTS - 1);

    dir = opendir(CROW_SEGMENT_DIR);
    if (!dir)
        return;

    while ((entry = readdir(dir)) != NULL) {
        if (!parse_segment_index(entry->d_name, &index))
            continue;

        if (index >= oldest_keep)
            continue;

        snprintf(path, sizeof(path), "%s/%s", CROW_SEGMENT_DIR, entry->d_name);
        if (unlink(path) != 0 && errno != ENOENT) {
            fprintf(stderr, "Crow: could not prune %s: %s\n",
                    path, strerror(errno));
        }
    }

    closedir(dir);
}

static void service_storage(struct event_state *event,
                            struct recording_state *recording,
                            int encoder_running)
{
    struct segment_scan scan;
    int64_t completed_max;

    if (scan_segments(&scan) != 0 || !scan.have_any)
        return;

    /* Highest numbered file is still open while the encoder is running. */
    completed_max = encoder_running
        ? ((int64_t)scan.max_index - 1)
        : (int64_t)scan.max_index;

    service_event(event, completed_max);
    service_recording(recording, completed_max);
    service_event_audio(event);
    service_recording_audio(recording);
    prune_ring(completed_max);
    prune_audio_ring();
}

static int count_numbered_dirs(const char *root, const char *prefix)
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;
    size_t prefix_len = strlen(prefix);

    dir = opendir(root);
    if (!dir)
        return 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefix_len) == 0)
            count++;
    }

    closedir(dir);
    return count;
}

static void write_status_json(const struct event_state *event,
                              const struct recording_state *recording,
                              int running)
{
    char temp_path[PATH_MAX];
    struct segment_scan scan;
    struct audio_scan audio;
    struct statvfs fs;
    int current_segment = -1;
    int completed_segment = -1;
    int rewind_seconds = 0;
    int audio_rewind_seconds = 0;
    double free_mib = 0.0;
    const char *hindsight_state = "idle";
    FILE *fp;

    if (scan_segments(&scan) == 0 && scan.have_any) {
        int retained_completed = scan.count > 0 ? scan.count - (running ? 1 : 0) : 0;
        if (retained_completed > CROW_RING_SEGMENTS)
            retained_completed = CROW_RING_SEGMENTS;
        current_segment = (int)scan.max_index;
        completed_segment = running ? (int)scan.max_index - 1 : (int)scan.max_index;
        rewind_seconds = retained_completed * CROW_SEGMENT_SECONDS;
    }

    memset(&audio, 0, sizeof(audio));
    if (scan_audio(&audio) == 0 && audio.have_any) {
        int completed_audio = audio.count > 0 ? audio.count - (audio_is_running() ? 1 : 0) : 0;
        if (completed_audio > CROW_AUDIO_RING_SEGMENTS)
            completed_audio = CROW_AUDIO_RING_SEGMENTS;
        audio_rewind_seconds = completed_audio * CROW_AUDIO_SEGMENT_SECONDS;
    }

    if (event->pending)
        hindsight_state = "pending";
    else if (event->active)
        hindsight_state = "active";

    if (statvfs("/root", &fs) == 0) {
        unsigned long long free_bytes =
            (unsigned long long)fs.f_bavail * (unsigned long long)fs.f_frsize;
        free_mib = (double)free_bytes / (1024.0 * 1024.0);
    }

    snprintf(temp_path, sizeof(temp_path), "%s.part", CROW_STATUS_FILE);
    fp = fopen(temp_path, "w");
    if (!fp)
        return;

    fprintf(fp,
        "{\n"
        "  \"version\": \"%s\",\n"
        "  \"running\": %s,\n"
        "  \"current_segment\": %d,\n"
        "  \"completed_segment\": %d,\n"
        "  \"rewind_seconds\": %d,\n"
        "  \"rewind_capacity_seconds\": %d,\n"
        "  \"hindsight\": {\"state\": \"%s\", \"event_id\": %d, \"pre_saved\": %d, \"post_saved\": %d},\n"
        "  \"recording\": {\"active\": %s, \"stopping\": %s, \"id\": %d, \"video_segments_saved\": %d, \"audio_segments_saved\": %d},\n"
        "  \"microphone\": {\"running\": %s, \"rewind_seconds\": %d},\n"
        "  \"saved_events\": %d,\n"
        "  \"saved_recordings\": %d,\n"
        "  \"free_mib\": %.1f\n"
        "}\n",
        CROW_VERSION,
        running ? "true" : "false",
        current_segment,
        completed_segment,
        rewind_seconds,
        CROW_RING_SECONDS,
        hindsight_state,
        event->event_id,
        event->pre_saved,
        event->post_saved,
        recording->active ? "true" : "false",
        recording->stop_pending ? "true" : "false",
        recording->recording_id,
        recording->segments_saved,
        recording->audio_segments_saved,
        audio_is_running() ? "true" : "false",
        audio_rewind_seconds,
        count_numbered_dirs(CROW_EVENT_DIR, "event_"),
        count_numbered_dirs(CROW_RECORDING_DIR, "recording_"),
        free_mib);

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    rename(temp_path, CROW_STATUS_FILE);
}

static void print_disk_status(void)
{
    struct statvfs fs;

    if (statvfs("/root", &fs) == 0) {
        unsigned long long free_bytes =
            (unsigned long long)fs.f_bavail * (unsigned long long)fs.f_frsize;
        unsigned long long total_bytes =
            (unsigned long long)fs.f_blocks * (unsigned long long)fs.f_frsize;
        printf("Filesystem free: %.1f MiB / %.1f MiB\n",
               (double)free_bytes / (1024.0 * 1024.0),
               (double)total_bytes / (1024.0 * 1024.0));
    }
}

static void print_status(const struct event_state *event,
                         const struct recording_state *recording)
{
    struct segment_scan scan;
    int64_t completed_max;
    int retained_completed;

    printf("\n=== Crow status ===\n");

    if (scan_segments(&scan) != 0 || !scan.have_any) {
        printf("Encoder starting; no segment files yet.\n");
    } else {
        completed_max = (int64_t)scan.max_index - 1;
        retained_completed = scan.count > 0 ? scan.count - 1 : 0;
        if (retained_completed > CROW_RING_SEGMENTS)
            retained_completed = CROW_RING_SEGMENTS;

        printf("Current segment: %06u\n", scan.max_index);
        if (completed_max >= 0)
            printf("Completed through: %06lld\n", (long long)completed_max);
        printf("Ring files on disk: %d\n", scan.count);
        printf("Approx rewind available: %d sec / %d sec\n",
               retained_completed * CROW_SEGMENT_SECONDS,
               CROW_RING_SECONDS);
    }

    if (event->pending) {
        printf("Hindsight: waiting for trigger segment %06u to close\n",
               event->trigger_index);
    } else if (event->active) {
        int remaining_segments = 0;
        if (event->post_goal_index >= event->next_post_index)
            remaining_segments = (int)(event->post_goal_index - event->next_post_index + 1);
        printf("Hindsight: event %04d active, %d post saved, ~%d sec remaining\n",
               event->event_id,
               event->post_saved,
               remaining_segments * CROW_SEGMENT_SECONDS);
    } else {
        printf("Hindsight: idle\n");
    }

    {
        struct audio_scan audio;
        if (scan_audio(&audio) == 0 && audio.have_any) {
            int completed_audio = audio.count > 0 ? audio.count - (audio_is_running() ? 1 : 0) : 0;
            if (completed_audio > CROW_AUDIO_RING_SEGMENTS)
                completed_audio = CROW_AUDIO_RING_SEGMENTS;
            printf("Microphone: %s, approx audio rewind %d sec\n",
                   audio_is_running() ? "running" : "stopped",
                   completed_audio * CROW_AUDIO_SEGMENT_SECONDS);
        } else {
            printf("Microphone: %s, no completed audio segments yet\n",
                   audio_is_running() ? "starting" : "unavailable");
        }
    }

    if (recording->active) {
        printf("Normal recording: %04d %s, %d video + %d audio segment%s saved\n",
               recording->recording_id,
               recording->stop_pending ? "stopping" : "recording",
               recording->segments_saved,
               recording->audio_segments_saved,
               recording->segments_saved == 1 ? "" : "s");
    } else {
        printf("Normal recording: idle\n");
    }

    printf("Saved Hindsight events: %d\n",
           count_numbered_dirs(CROW_EVENT_DIR, "event_"));
    printf("Saved normal recordings: %d\n",
           count_numbered_dirs(CROW_RECORDING_DIR, "recording_"));
    print_disk_status();
    printf("===================\n");
}

int main(void)
{
    pthread_t encoder_thread;
    sigset_t control_signals;
    sigset_t old_mask;
    int stop_requested = 0;
    int stop_wake_started = 0;
    uint64_t stop_request_ms = 0;
    uint64_t last_status_write_ms = 0;
    pthread_t stop_wake_thread;
    int pthread_result;
    struct event_state event;
    struct recording_state recording;

    memset(&event, 0, sizeof(event));
    memset(&recording, 0, sizeof(recording));

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("Crow Camera v%s starting...\n", CROW_VERSION);
    printf("2560x1440 H.265 at %d FPS, 20 Mbps CBR quality profile.\n", CROW_FPS);
    printf("Raw capture stays full-width; Crow media export defaults to a centered 1920x1440 (4:3) view.\n");
    printf("2-second independent H.265 segments.\n");
    printf("Rolling rewind buffer: ~%d seconds (%d segments).\n",
           CROW_RING_SECONDS, CROW_RING_SEGMENTS);
    printf("Hindsight: ~%d sec before + ~%d sec after trigger; retrigger extends tail.\n",
           CROW_RING_SECONDS, CROW_POST_SECONDS);
    printf("Normal recording mode is enabled in parallel with the rolling buffer.\n");
    printf("Onboard microphone audio: 48 kHz mono PCM ring; app/media clients can fetch video + audio over Wi-Fi.\n");
    printf("Crow v0.9 publishes a JSON status/media API for the phone app.\n");
    printf("Ctrl+C/SIGTERM = clean stop | SIGUSR1 = Hindsight | SIGUSR2 = status | SIGHUP = record toggle\n");

    if (chdir("/root") != 0) {
        fprintf(stderr, "Could not chdir to /root: %s\n", strerror(errno));
        return 1;
    }

    if (prepare_segment_dir() != 0)
        return 1;

    if (ensure_dir(CROW_EVENT_DIR) != 0)
        return 1;

    if (ensure_dir(CROW_RECORDING_DIR) != 0)
        return 1;

    (void)start_audio_capture();

    char *argv[] = {
        "crow_camera",

        "-c", "265",
        "-w", "2560",
        "-h", "1440",
        "-o", CROW_FIRST_OUTPUT,

        "--testMode=2",
        "--sensorEn=1",
        "--bindmode=1",
        "--numChn=1",

        "--viWidth=2560",
        "--viHeight=1440",
        "--vpssWidth=2560",
        "--vpssHeight=1440",

        "--srcFramerate=25",
        "--framerate=25",

        "--rcMode=0",
        "--bitrate=20000",
        "--gop=50",
        "--getstream-timeout=500",

        "-n", "1000000000",

        NULL
    };

    int argc = 0;
    while (argv[argc] != NULL)
        argc++;

    sigemptyset(&control_signals);
    sigaddset(&control_signals, SIGINT);
    sigaddset(&control_signals, SIGTERM);
    sigaddset(&control_signals, SIGUSR1);
    sigaddset(&control_signals, SIGUSR2);
    sigaddset(&control_signals, SIGHUP);

    pthread_result = pthread_sigmask(SIG_BLOCK, &control_signals, &old_mask);
    if (pthread_result != 0) {
        fprintf(stderr, "Could not block control signals: %s\n",
                strerror(pthread_result));
        return 1;
    }

    struct encoder_job job = {
        .argc = argc,
        .argv = argv,
        .result = 1,
        .done = 0,
        .lock = PTHREAD_MUTEX_INITIALIZER,
    };

    pthread_result = pthread_create(
        &encoder_thread,
        NULL,
        encoder_thread_main,
        &job
    );

    if (pthread_result != 0) {
        fprintf(stderr, "Could not start encoder thread: %s\n",
                strerror(pthread_result));
        pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        stop_audio_capture();
        return 1;
    }

    while (!encoder_job_done(&job)) {
        struct timespec timeout = {
            .tv_sec = 0,
            .tv_nsec = 100000000L,
        };
        int signo;

        service_storage(&event, &recording, 1);

        {
            uint64_t now_ms = monotonic_ms();
            if (now_ms - last_status_write_ms >= 1000) {
                write_status_json(&event, &recording, 1);
                last_status_write_ms = now_ms;
            }

            if (stop_requested && !encoder_job_done(&job)) {
                uint64_t elapsed_ms = now_ms - stop_request_ms;

                if (!stop_wake_started && elapsed_ms >= 500) {
                    int wake_result = pthread_create(
                        &stop_wake_thread, NULL, encoder_stop_wake_thread, NULL);
                    if (wake_result == 0) {
                        pthread_detach(stop_wake_thread);
                        stop_wake_started = 1;
                        printf("Crow shutdown: encoder wake helper started.\n");
                    } else {
                        fprintf(stderr, "Crow shutdown: could not start wake helper: %s\n",
                                strerror(wake_result));
                        stop_wake_started = 1;
                    }
                }

                if (elapsed_ms >= 5000) {
                    fprintf(stderr,
                            "Crow shutdown watchdog: encoder did not exit after 5 sec; "
                            "forcing process exit to avoid an indefinite hang.\n");
                    stop_audio_capture();
                    write_status_json(&event, &recording, 0);
                    fflush(NULL);
                    sync();
                    _exit(0);
                }
            }
        }

        signo = sigtimedwait(&control_signals, NULL, &timeout);

        if (signo == SIGINT || signo == SIGTERM) {
            if (!stop_requested) {
                stop_requested = 1;
                stop_request_ms = monotonic_ms();
                printf("\nStop requested. Finalizing current segment...\n");
                crow_request_stop();
            }
        } else if (signo == SIGUSR1) {
            request_hindsight(&event);
            service_storage(&event, &recording, 1);
        } else if (signo == SIGUSR2) {
            print_status(&event, &recording);
        } else if (signo == SIGHUP) {
            request_record_toggle(&recording);
            service_storage(&event, &recording, 1);
        } else if (signo == -1 && errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "sigtimedwait failed: %s\n", strerror(errno));
            crow_request_stop();
            stop_requested = 1;
            stop_request_ms = monotonic_ms();
        }
    }

    pthread_result = pthread_join(encoder_thread, NULL);
    if (pthread_result != 0) {
        fprintf(stderr, "Could not join encoder thread: %s\n",
                strerror(pthread_result));
        return 1;
    }

    /* The final open video segment is complete now. */
    service_storage(&event, &recording, 0);

    /* Close the current microphone segment so partial audio is not lost. */
    stop_audio_capture();
    service_event_audio(&event);
    service_recording_audio(&recording);

    if (event.active)
        finish_event(&event, "stopped_before_post_complete");

    if (recording.active)
        finish_recording(&recording, "stopped_while_recording");

    if (job.result != 0) {
        fprintf(stderr, "Crow encoder returned: %d\n", job.result);
        return job.result;
    }

    {
        struct segment_scan scan;
        int ring_files = 0;
        if (scan_segments(&scan) == 0 && scan.have_any)
            ring_files = scan.count;

        printf("Crow stopped cleanly.\n");
        printf("Rolling ring contains %d H.265 segment%s in %s\n",
               ring_files, ring_files == 1 ? "" : "s", CROW_SEGMENT_DIR);
        printf("Saved Hindsight event directories: %d in %s\n",
               count_numbered_dirs(CROW_EVENT_DIR, "event_"), CROW_EVENT_DIR);
        printf("Saved normal recording directories: %d in %s\n",
               count_numbered_dirs(CROW_RECORDING_DIR, "recording_"), CROW_RECORDING_DIR);
    }

    write_status_json(&event, &recording, 0);
    return 0;
}
