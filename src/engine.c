/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "capture_ns.h"
#include <ane.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The only accepted DF archive is the audited, public DFN3 export. In
 * particular do not pass an arbitrary archive to Rust's aborting C API. */
#define DF_SHA "c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616"
#define RN_SHA "6b8943dc4a9b6b24425873992a44f29c0577503276456af46a8854774faeb294"

struct ns_engine {
    enum ns_model model;
    void *library, *state;
    void *rn_model;
    void *(*rn_load_model)(FILE *);
    void (*rn_free_model)(void *);
    void *(*rn_create)(void *);
    int (*rn_frame_size)(void);
    float (*rn_process)(void *, float *, const float *);
    void (*destroy)(void *);
    void *(*df_create)(const char *, float, const char *);
    size_t (*df_frame_size)(void *);
    float (*df_process)(void *, float *, float *);
    struct ane_fallback_state fallback;
    const float *input;
    float *output;
    uint64_t cpu_ns;
};

static FILE *checked_file(const char *path, const char *expected)
{
    FILE *f;
    EVP_MD_CTX *ctx;
    unsigned char buf[8192], digest[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    size_t count;
    char hex[65];
    int ok = 0;
    int fd;
    struct stat st;
    if (!path || (fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 64*1024*1024) {
        close(fd);
        return NULL;
    }
    if (!(f = fdopen(fd, "rb"))) { close(fd); return NULL; }
    ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) goto done;
    while ((count = fread(buf, 1, sizeof(buf), f)))
        if (EVP_DigestUpdate(ctx, buf, count) != 1) goto done;
    if (ferror(f) || EVP_DigestFinal_ex(ctx, digest, &n) != 1 || n != 32) goto done;
    for (unsigned int i = 0; i < n; i++) snprintf(hex + 2*i, 3, "%02x", digest[i]);
    ok = strcmp(hex, expected) == 0;
done:
    EVP_MD_CTX_free(ctx);
    if (!ok || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    return f;
}

#define SYMBOL(field, name) do { \
    *(void **)(&e->field) = dlsym(e->library, name); \
    if (!e->field) goto fail; \
} while (0)

struct ns_engine *ns_engine_new(enum ns_model model)
{
    struct ns_engine *e = calloc(1, sizeof(*e));
    const char *library, *path;
    FILE *model_file = NULL;
    if (!e) return NULL;
    e->model = model;
    if (model == NS_RNNOISE) {
        path = getenv("AURORA_RNNOISE_MODEL");
        if (!(model_file = checked_file(path, RN_SHA))) {
            fprintf(stderr, "capture-ns: AURORA_RNNOISE_MODEL must name the audited v0.1.1 model (sha256 " RN_SHA ")\n");
            goto fail;
        }
        library = getenv("AURORA_RNNOISE_LIBRARY");
        e->library = dlopen(library ? library : "librnnoise.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!e->library) goto fail;
        SYMBOL(rn_create, "rnnoise_create");
        SYMBOL(rn_frame_size, "rnnoise_get_frame_size");
        SYMBOL(rn_process, "rnnoise_process_frame");
        SYMBOL(destroy, "rnnoise_destroy");
        SYMBOL(rn_load_model, "rnnoise_model_from_file");
        SYMBOL(rn_free_model, "rnnoise_model_free");
        if (e->rn_frame_size() != (int)NS_HOP) goto fail;
        e->rn_model = e->rn_load_model(model_file);
        fclose(model_file);
        model_file = NULL;
        if (!e->rn_model || !(e->state = e->rn_create(e->rn_model))) goto fail;
    } else if (model == NS_DF3) {
        char fd_path[64];
        path = getenv("AURORA_DF_MODEL");
        if (!(model_file = checked_file(path, DF_SHA))) {
            fprintf(stderr, "capture-ns: AURORA_DF_MODEL must name the audited DFN3 archive (sha256 " DF_SHA ")\n");
            goto fail;
        }
        library = getenv("AURORA_DF_LIBRARY");
        e->library = dlopen(library ? library : "libdf.so", RTLD_NOW | RTLD_LOCAL);
        if (!e->library) goto fail;
        SYMBOL(df_create, "df_create");
        SYMBOL(df_frame_size, "df_get_frame_length");
        SYMBOL(df_process, "df_process_frame");
        SYMBOL(destroy, "df_free");
        /* Reopen the verified inode, rather than resolving a mutable path a
         * second time. Linux is the supported target for this plugin. */
        snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fileno(model_file));
        e->state = e->df_create(fd_path, 100.0f, NULL);
        fclose(model_file);
        model_file = NULL;
        if (!e->state || e->df_frame_size(e->state) != NS_HOP) goto fail;
    } else goto fail;
    /* No native model is advertised until the complete graph is compiled,
     * resident and parity-qualified. No device probing or per-app scheduler. */
    ane_fallback_init(&e->fallback, 0);
    return e;
fail:
    if (model_file) fclose(model_file);
    fprintf(stderr, "capture-ns: model initialization failed; no source created\n");
    ns_engine_free(e);
    return NULL;
}

void ns_engine_free(struct ns_engine *e)
{
    if (!e) return;
    if (e->state && e->destroy) e->destroy(e->state);
    if (e->rn_model && e->rn_free_model) e->rn_free_model(e->rn_model);
    if (e->library) dlclose(e->library);
    free(e);
}

static int cpu(void *context)
{
    struct ns_engine *e = context;
    float in[NS_HOP];
    float score;
    if (e->model == NS_RNNOISE) {
        for (unsigned int i = 0; i < NS_HOP; i++) in[i] = e->input[i] * 32768.0f;
        score = e->rn_process(e->state, e->output, in);
        for (unsigned int i = 0; i < NS_HOP; i++) e->output[i] /= 32768.0f;
    } else {
        memcpy(in, e->input, sizeof(in));
        score = e->df_process(e->state, in, e->output);
    }
    if (!isfinite(score)) return -EIO;
    for (unsigned int i = 0; i < NS_HOP; i++)
        if (!isfinite(e->output[i])) return -EIO;
    return 0;
}

static uint64_t thread_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}

int ns_engine_process(struct ns_engine *e, const float in[NS_HOP], float out[NS_HOP])
{
    uint64_t start;
    int rc;
    if (!e || !in || !out) return -EINVAL;
    for (unsigned int i = 0; i < NS_HOP; i++)
        if (!isfinite(in[i]) || fabsf(in[i]) > 1.0f) {
            memset(out, 0, NS_HOP * sizeof(float));
            return -EINVAL;
        }
    e->input = in;
    e->output = out;
    start = thread_ns();
    rc = ane_fallback_run(&e->fallback, NULL, cpu, e, NULL);
    e->cpu_ns += thread_ns() - start;
    if (rc) memset(out, 0, NS_HOP * sizeof(float));
    return rc;
}

void ns_engine_stats(const struct ns_engine *e, struct ns_stats *s)
{
    *s = (struct ns_stats) { e->fallback.calls, e->fallback.cpu_calls,
        e->fallback.native_calls, e->fallback.failed_calls,
        e->fallback.total_ns, e->fallback.max_ns, e->cpu_ns, e->fallback.last_ns };
}
