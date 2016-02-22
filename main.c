#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>
#include <assert.h>

#if 0
#define VLOG printf
#else
#define VLOG if (0) printf
#endif

#define LOG printf

#define CURL_CALL(op)                                       \
    {                                                       \
        const CURLcode res = op;                            \
        if (res) {                                          \
            fprintf(stderr, "Curl easy failure: %s (%s)\n", \
                    curl_easy_strerror(res), #op);          \
            return res;                                     \
        }                                                   \
    }

#define CURLM_CALL(op)                                          \
    {                                                           \
        const CURLMcode res = op;                               \
        if (res) {                                              \
            fprintf(stderr, "Curl multi failure: %s (%s)\n",    \
                    curl_multi_strerror(res), #op);             \
            return res;                                         \
        }                                                       \
    }

static size_t headerCallback(void * dataPtr, size_t size, size_t nmemb, void *userdata)
{
    size *= nmemb;
    size_t colon = 0;
    const char *data = (const char*)dataPtr;
    while (colon < size) {
        if (data[colon] == ':') {
            break;
        } else if (isspace(data[colon])) {
            char *httpVersion = (char*)(userdata);
            if (size >= 8 && !strncmp(data, "HTTP/", 5)) {
                strncpy(httpVersion, data + 5, 3);
            }
            break;
        }
        ++colon;
    }
    return size;
}

static size_t dataCallback(void * data, size_t size, size_t nmemb, void * userdata)
{
    *((int*)userdata) += (size * nmemb);
    VLOG("GOT DATA %d\n", *((int*)userdata));
    (void)data;
    (void)userdata;
    return size * nmemb;
}

struct Request {
    const char *url;
    struct Request *next;
    char httpVersion[4];
    CURL *easy;
    int written;
};

static struct Request *create(struct Request *head, const char *url)
{
    struct Request *req = (struct Request*)calloc(sizeof(struct Request), 1);
    req->url = url;
    if (!head) {
        head = req;
    } else {
        struct Request *tmp = head;
        while (tmp->next)
            tmp = tmp->next;
        tmp->next = req;
    }
    return head;
}

static int addRequest(CURLM *multi, struct Request **reqPtr)
{
    struct Request *req = *reqPtr;
    req->easy = curl_easy_init();
    CURL_CALL(curl_easy_setopt(req->easy, CURLOPT_URL, req->url));
    LOG("SETTING URL %s - %p\n", req->url, req->easy);
    CURL_CALL(curl_easy_setopt(req->easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0));
    CURL_CALL(curl_easy_setopt(req->easy, CURLOPT_HEADERFUNCTION, headerCallback));
    CURL_CALL(curl_easy_setopt(req->easy, CURLOPT_HEADERDATA, &req->httpVersion));
    CURL_CALL(curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, dataCallback));
    CURL_CALL(curl_easy_setopt(req->easy, CURLOPT_SSL_VERIFYHOST, 0));
    CURL_CALL(curl_easy_setopt(req->easy, CURLOPT_SSL_VERIFYPEER, 0));
    CURL_CALL(curl_easy_setopt(req->easy, CURLOPT_PIPEWAIT, 1));
    CURL_CALL(curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, &req->written));
    /* CURL_CALL(curl_easy_setopt(req->easy, CURLOPT_VERBOSE, 1)); */
    CURLM_CALL(curl_multi_add_handle(multi, req->easy));
    *reqPtr = req->next;
    return CURLE_OK;
}

static int process(CURLM *multi, struct Request **head)
{
    int done;
    do {
        done = 0;
        fd_set r, w;
        FD_ZERO(&r);
        FD_ZERO(&w);
        int maxFD = 0;
        CURLM_CALL(curl_multi_fdset(multi, &r, &w, 0, &maxFD));

        long maxTime = -1;
        CURL_CALL(curl_multi_timeout(multi, &maxTime));
        struct timeval t = { 0, 1000 };
        if (maxTime > 0) {
            t.tv_sec = maxTime / 1000;
            t.tv_usec = (maxTime % 1000) * 1000;
        }
        VLOG("maxtime %ld\n", maxTime);

        if (maxFD > 0) {
            VLOG("SELECTING for %ld.%03ld (%ld)\n",
                 t.tv_sec, t.tv_usec / 1000, maxTime);
            const int ret = select(maxFD , &r, &w, 0, &t);
            if (ret == -1 && errno == EINTR)
                continue;
        }

        CURLMcode m;
        do {
            m = curl_multi_perform(multi, 0);
        } while (m == CURLM_CALL_MULTI_PERFORM);

        if (m) {
            fprintf(stderr, "Curl multi perform failure: %s\n", curl_multi_strerror(m));
            return m;
        }

        CURLMsg *msg;
        while ((msg = curl_multi_info_read(multi, 0)) != 0) {
            if (msg->msg == CURLMSG_DONE) {
                struct Request *req = *head;
                struct Request *prev = 0;
                while (req && req->easy != msg->easy_handle) {
                    prev = req;
                    req = req->next;
                }

                if (prev) {
                    assert(req != *head);
                    prev->next = req->next;
                } else {
                    assert(req == *head);
                    *head = req->next;
                }
                if (msg->data.result) {
                    LOG("Request failed %s %s\n", req->url,
                        curl_easy_strerror(msg->data.result));
                } else {
                    long status;
                    CURL_CALL(curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &status));
                    LOG("Request finished %s %s => %ld\n", req->url,
                        req->httpVersion, status);
                }
                /* VLOG("ABOUT TO CLEANUP %p\n", ((struct SessionHandle*)easy->easy_conn); */
                curl_multi_remove_handle(multi, msg->easy_handle);
                curl_easy_cleanup(msg->easy_handle);
                free(req);
                done = 1;
                break;
            }
        }
    } while (!done);

    return CURLE_OK;
}

int main(int argc, char **argv)
{
    const char *urls[] = {
        "https://dtaserver.corp.netflix.com:8081/files/data-1k",
        "https://dtaserver.corp.netflix.com:8081/files/data-10k",
        "https://dtaserver.corp.netflix.com:8081/files/data-100k",
        "https://dtaserver.corp.netflix.com:8081/files/data-1m",
        "https://dtaserver.corp.netflix.com:8081/files/data-10m",
        "https://dtaserver.corp.netflix.com:8081/files/data-50m"
    };
    enum {
        Parallel,
        Sequential
    } mode = Parallel;
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--parallel") || !strcmp(argv[i], "-p")) {
            mode = Parallel;
        } else if (!strcmp(argv[i], "--sequential") || !strcmp(argv[i], "-s")) {
            mode = Sequential;
        }
    }

    struct Request *head = 0;
    const size_t count = sizeof(urls) / sizeof(urls[0]);
    for (size_t i=0; count; ++i) {
        head = create(head, urls[i]);
    }
    struct Request *next = head;

    CURLM *multi = curl_multi_init();
    CURLM_CALL(curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_HTTP1|CURLPIPE_MULTIPLEX));
    if (mode == Parallel) {
        while (next) {
            if (addRequest(multi, &next) != 0)
                return 1;
        }
        while (head) {
            if (process(multi, &head)) {
                return 1;
            }
        }
    } else {
        while (next) {
            if (addRequest(multi, &next) != 0)
                return 1;
            if (process(multi, &head))
                return 1;
        }
    }

    curl_multi_cleanup(multi);
    return 0;
}
