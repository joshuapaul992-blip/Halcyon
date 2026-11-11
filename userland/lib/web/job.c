/* libweb: a request in a worker thread, with cancellation. The worker owns
 * a copy of the request; whoever of the two (worker, web_job_free) comes
 * last frees the job. */
#include "webint.h"

struct web_job {
    web_session_t *s;
    web_request_t rq;
    char *url, *method, *ctype, *referer, *accept;
    void *body;
    int (*user_data)(void *, const void *, size_t);
    void (*user_progress)(void *, const char *, long long, long long);
    void *user_ctx;
    web_response_t rs;
    volatile int cancel;
    volatile int done;
    int orphan;                 /* web_job_free was called while it ran */
    wp_mutex_t lock;
};

static void job_destroy(web_job_t *j) {
    web_response_free(&j->rs);
    free(j->url); free(j->method); free(j->ctype); free(j->referer); free(j->accept); free(j->body);
    free(j);
}

/* the caller's callbacks, unless the caller has let go of the job */
static int job_on_data(void *ctx, const void *data, size_t n) {
    web_job_t *j = ctx;
    if (j->orphan || j->cancel) return WEB_E_CANCELED;
    return j->user_data(j->user_ctx, data, n);
}
static void job_on_progress(void *ctx, const char *phase, long long got, long long total) {
    web_job_t *j = ctx;
    if (!j->orphan && !j->cancel) j->user_progress(j->user_ctx, phase, got, total);
}
static int worker(void *arg) {
    web_job_t *j = arg;
    web_fetch(j->s, &j->rq, &j->rs);
    wp_lock(&j->lock);
    j->done = 1;
    int orphan = j->orphan;
    wp_unlock(&j->lock);
    if (orphan) job_destroy(j);
    return 0;
}

static char *dup_or_null(const char *s, int *fail) {
    if (!s) return 0;
    char *d = w_strndup(s, strlen(s));
    if (!d) *fail = 1;
    return d;
}

web_job_t *web_job_start(web_session_t *s, const web_request_t *rq) {
    web_job_t *j = calloc(1, sizeof *j);
    if (!j) return 0;
    int fail = 0;
    j->s = s;
    j->rq = *rq;
    j->url = dup_or_null(rq->url, &fail);
    j->method = dup_or_null(rq->method, &fail);
    j->ctype = dup_or_null(rq->content_type, &fail);
    j->referer = dup_or_null(rq->referer, &fail);
    j->accept = dup_or_null(rq->accept, &fail);
    if (rq->body && rq->body_len) {
        j->body = malloc(rq->body_len);
        if (j->body) memcpy(j->body, rq->body, rq->body_len); else fail = 1;
    }
    if (fail) { job_destroy(j); return 0; }
    j->rq.url = j->url;
    j->rq.method = j->method;
    j->rq.content_type = j->ctype;
    j->rq.referer = j->referer;
    j->rq.accept = j->accept;
    j->rq.body = j->body;
    j->rq.cancel = &j->cancel;
    j->user_data = rq->on_data;
    j->user_progress = rq->on_progress;
    j->user_ctx = rq->ctx;
    j->rq.on_data = rq->on_data ? job_on_data : 0;
    j->rq.on_progress = rq->on_progress ? job_on_progress : 0;
    j->rq.ctx = j;
    if (wp_thread(worker, j)) { job_destroy(j); return 0; }
    return j;
}

int web_job_done(web_job_t *j) { return j->done; }
void web_job_cancel(web_job_t *j) { j->cancel = 1; }

int web_job_wait(web_job_t *j, int timeout_ms) {
    uint64_t end = wp_ms() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
    while (!j->done) {
        if (timeout_ms >= 0 && wp_ms() >= end) return 0;
        wp_sleep(5);
    }
    return 1;
}

web_response_t *web_job_response(web_job_t *j) { return j->done ? &j->rs : 0; }

void web_job_free(web_job_t *j) {
    if (!j) return;
    wp_lock(&j->lock);
    int done = j->done;
    if (!done) { j->orphan = 1; j->cancel = 1; }
    wp_unlock(&j->lock);
    if (done) job_destroy(j);
}
