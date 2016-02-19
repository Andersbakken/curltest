#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>

struct Request
{
    const char *url;
    int http2;
};

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
    printf("GOT DATA %d\n", *((int*)userdata));
    (void)data;
    (void)userdata;
    return size * nmemb;
}


int main(int argc, char **argv)
{
    size_t max = 1024;
    if (argc > 1)
        max = atoi(argv[1]);
    struct Request {
        const char *url;
        int http2;
    } requests[] = {
        { "https://lgud-hkhan2.corp.netflix.com:8081/files/data-100k", 1 },
        { "https://lgud-hkhan2.corp.netflix.com:8081/files/data-100k", 1 },
        { "http://www.vg.no", 0 }
        /* { "http://test.example.org:12345/files/data-1m", 1 }, */
        /* { "http://test.example.org:12345/files/data-100k", 0 } */
    };
    CURLM *multi = curl_multi_init();
    CURLM_CALL(curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_HTTP1|CURLPIPE_MULTIPLEX));

    size_t idx;
    CURL *last = 0;
    for (idx=0; idx<max; ++idx) {
        const struct Request *req = &requests[idx % (sizeof(requests) / sizeof(requests[0]))];
        CURL *easy = curl_easy_init();
        if (last) {
            curl_easy_cleanup(last);
        }
        last = easy;
        CURL_CALL(curl_easy_setopt(easy, CURLOPT_URL, req->url));
        printf("SETTING URL %s - %d - %p\n", req->url, req->http2, easy);
        if (req->http2)
            CURL_CALL(curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0));
        CURL_CALL(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, headerCallback));
        char httpVersion[4];
        memset(&httpVersion, 0, sizeof(httpVersion));
        CURL_CALL(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &httpVersion));
        CURL_CALL(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, dataCallback));
        CURL_CALL(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0));
        CURL_CALL(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0));

        int written = 0;
        CURL_CALL(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &written));
        /* CURL_CALL(curl_easy_setopt(easy, CURLOPT_VERBOSE, 1)); */
        CURLM_CALL(curl_multi_add_handle(multi, easy));

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
            /* printf("maxtime %ld: %d %\n", maxTime); */

            if (maxFD > 0) {
                /* printf("SELECTING for %ld.%03ld (%ld)\n", */
                /*        t.tv_sec, t.tv_usec / 1000, maxTime); */
                const int ret = select(maxFD , &r, &w, 0, &t);
                if (ret == -1 && errno == EINTR)
                    continue;
            }

            CURLMcode m;
            int running = 0;
            do {
                m = curl_multi_perform(multi, &running);
            } while (m == CURLM_CALL_MULTI_PERFORM);

            if (m) {
                fprintf(stderr, "Curl multi perform failure: %s\n", curl_multi_strerror(m));
                return m;
            }

            if (!running) {
                int remaining;
                CURLMsg *msg;
                while ((msg = curl_multi_info_read(multi, &remaining)) != 0) {
                    if (msg->msg == CURLMSG_DONE) {
                        if (msg->data.result) {
                            fprintf(stderr, "Request failed %s%s %s\n", req->url,
                                    req->http2 ? " (http2)" : "",
                                    curl_easy_strerror(msg->data.result));
                        } else {
                            long status;
                            CURL_CALL(curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status));
                            printf("Request finished %s%s %s => %ld\n", req->url,
                                   req->http2 ? " (http2)" : "", httpVersion, status);
                        }
                        /* printf("ABOUT TO CLEANUP %p\n", ((struct SessionHandle*)easy->easy_conn); */
                        curl_multi_remove_handle(multi, easy);
                        done = 1;
                    }
                }
            }
        } while (!done);
    }
    if (last)
        curl_easy_cleanup(last);
    curl_multi_cleanup(multi);
    return 0;
}
