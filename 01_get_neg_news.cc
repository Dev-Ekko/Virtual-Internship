/*
Author: Ekko(dev.dongningyuan@gmail.com)

Reference Library:
   1.Sogou C++ Workflow
   2.libcurl
   3.libxml2
   4.nlohmann/json(JSON for Modern C++)

 GNU C++ compile command line suggestion (edit paths accordingly):
 g++ -std=c++11 -I /opt/sogou/include/ -I/usr/local/libxml2/include/libxml2/ -o 01_get_neg_news 01_get_neg_news.cc /opt/sogou/lib64/libworkflow.a -lssl -lpthread -lcrypto -lcurl -lxml2
*/
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFServer.h"
#include "workflow/WFHttpServer.h"
#include "workflow/WFFacilities.h"
#include <iostream>
#include <vector>
#include <memory>
#include <cstring>
#include <math.h>
#include <locale>
#include <codecvt>

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include "nlohmann/json.hpp"

using json = nlohmann::json;
using Converter = std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t>; 

using namespace std;

#ifdef _MSC_VER
#define COMPARE(a, b) (!stricmp((a), (b)))
#else
#define COMPARE(a, b) (!strcasecmp((a), (b)))
#endif

typedef struct TitleUrlDefined
{
    string title;
    string url;
} TitleUrl;

typedef struct ContextDefined
{
    ContextDefined(): addTitle(false) { }
    bool addTitle;
    string title;
    string url;
    vector<TitleUrl> terms;
} Context;

struct MemoryStruct {
  char *memory;
  size_t size;
};

static char errorBuffer[CURL_ERROR_SIZE];
static string buffer;

static int Writer(char *, size_t, size_t, string *);
static bool Init(CURL *&, char *);
static void ParseHtml(const string &, vector<TitleUrl> &);
static void StartElement(void *, const xmlChar *, const xmlChar **);
static void EndElement(void *, const xmlChar *);
static void Characters(void *, const xmlChar *, int);
static void CdataBlock(void *, const xmlChar *, int);

/* Call API */
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp);
static std::string CallApi(char* title);
static double CalculateTitle(char* title);

/* Get baidu news title */
static json GetNegNewsInfo();

/* Common */
static bool IsLessThan(double a,double b);

void process(WFHttpTask *server_task)
{
    json res = GetNegNewsInfo();

    protocol::HttpRequest *req = server_task->get_req();
	protocol::HttpResponse *resp = server_task->get_resp();
	long long seq = server_task->get_task_seq();
	protocol::HttpHeaderCursor cursor(req);
	std::string name;
	std::string value;
	char buf[8192];
	int len;

	/* Set response message body. */
    resp->append_output_body_nocopy("<html>", 6);
    len = snprintf(buf, 8192, "<p>%s %s %s</p>", req->get_method(),
    req->get_request_uri(), req->get_http_version());
    resp->append_output_body(buf, len);
    
    while (cursor.next(name, value))
    {
        len = snprintf(buf, 8192, "<p>%s: %s</p>", name.c_str(), value.c_str());
        resp->append_output_body(buf, len);
    } 

    /* Print negtive news data */
    for (json::iterator it = res.begin(); it != res.end(); ++it) 
    {
      std::string jsonKey = it.key();
      std::string jsonValue = res[it.key()].get<string>();
      len = snprintf(buf, 8192, "<p>{ %s: %s }</p>", jsonKey.c_str(),jsonValue.c_str()); 
      resp->append_output_body(buf, len);
    }	
    resp->append_output_body_nocopy("</html>", 7);

    /* Set status line if you like. */
    resp->set_http_version("HTTP/1.1");
    resp->set_status_code("200");
    resp->set_reason_phrase("OK");

    resp->add_header_pair("Content-Type", "text/html;;charset=UTF-8");
    resp->add_header_pair("Server", "Sogou WFHttpServer");
    if (seq == 9) /* no more than 10 requests on the same connection. */
        resp->add_header_pair("Connection", "close");

    /* print some log */
    char addrstr[128];
    struct sockaddr_storage addr;
    socklen_t l = sizeof addr;
    unsigned short port = 0;

    server_task->get_peer_addr((struct sockaddr *)&addr, &l);
    if (addr.ss_family == AF_INET)
	{
		struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
		inet_ntop(AF_INET, &sin->sin_addr, addrstr, 128);
		port = ntohs(sin->sin_port);
	}
	else if (addr.ss_family == AF_INET6)
	{
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&addr;
		inet_ntop(AF_INET6, &sin6->sin6_addr, addrstr, 128);
		port = ntohs(sin6->sin6_port);
	}
	else
		strcpy(addrstr, "Unknown");

	fprintf(stderr, "Peer address: %s:%d, seq: %lld.\n",
			addrstr, port, seq);
}

static WFFacilities::WaitGroup wait_group(1);

void sig_handler(int signo)
{
	wait_group.done();
}

int main(int argc, char* argv[])
{
    unsigned short port;

    if (argc != 2)
    {
        fprintf(stderr, "USAGE: %s <port>\n", argv[0]);
        exit(1);
    }

    signal(SIGINT, sig_handler);

    WFHttpServer server(process);
    port = atoi(argv[1]);
    if (server.start(port) == 0)
    {
        wait_group.wait();
        server.stop();
    }
    else
    {
        perror("Cannot start server");
        exit(1);
    }
    return 0;
}

static bool Init(CURL *&conn, char *url)
{
    CURLcode code;
    conn = curl_easy_init();
    if (conn == NULL)
    {
        fprintf(stderr, "Failed to create CURL connection/n");
        exit(EXIT_FAILURE);
    }
    code = curl_easy_setopt(conn, CURLOPT_ERRORBUFFER, errorBuffer);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "Failed to set error buffer [%d]/n", code);
        return false;
    }
    code = curl_easy_setopt(conn, CURLOPT_URL, url);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "Failed to set URL [%s]/n", errorBuffer);
        return false;
    }
    code = curl_easy_setopt(conn, CURLOPT_FOLLOWLOCATION, 1);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "Failed to set redirect option [%s]/n", errorBuffer);
        return false;
    }
    code = curl_easy_setopt(conn, CURLOPT_WRITEFUNCTION, Writer);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "Failed to set writer [%s]/n", errorBuffer);
        return false;
    }
    code = curl_easy_setopt(conn, CURLOPT_WRITEDATA, &buffer);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "Failed to set write data [%s]/n", errorBuffer);
        return false;
    }
    return true;
}

static int Writer(char *data, size_t size, size_t nmemb, string *writerData)
{
    unsigned long long sizes = size * nmemb;
    if (writerData == NULL) return 0;
    writerData->append(data, sizes);
    return sizes;
}

static htmlSAXHandler saxHandler =
{
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    StartElement,
    EndElement,
    NULL,
    Characters,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    CdataBlock,
    NULL
};

static void ParseHtml(const string &html, vector<TitleUrl> &arr)
{
    htmlParserCtxtPtr ctxt;
    Context context;
    ctxt = htmlCreatePushParserCtxt(&saxHandler, &context, "", 0, "", XML_CHAR_ENCODING_NONE);
    htmlParseChunk(ctxt, html.c_str(), html.size(), 0);
    htmlParseChunk(ctxt, "", 0, 1);
    htmlFreeParserCtxt(ctxt);
    arr = context.terms;
}

static void StartElement(void *voidContext, const xmlChar *name, const xmlChar **attributes)
{
    Context *context = (Context *)voidContext;
    if (COMPARE((char *)name, "a"))
    {
        context->url = (char *)attributes[1];
        context->title = "";
        context->addTitle = true;
    }
}

static void EndElement(void *voidContext, const xmlChar *name)
{
    Context *context = (Context *)voidContext;
    if (COMPARE((char *)name, "a"))
    {
        context->url = "";
        context->addTitle = false;
    }
}

static void handleCharacters(Context *context, const xmlChar *chars, int length)
{
    TitleUrl linkString;
    if (context->addTitle)
    {
        context->title.append((char *)chars, length);
        linkString.title = context->title;
        linkString.url = context->url;
        context->terms.push_back(linkString);
    }
}

static void Characters(void *voidContext, const xmlChar *chars, int length)
{
    Context *context = (Context *)voidContext;
    handleCharacters(context, chars, length);
}

static void CdataBlock(void *voidContext, const xmlChar *chars, int length)
{
    Context *context = (Context *)voidContext;
    handleCharacters(context, chars, length);
}

/* Call API */
static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;

  char *ptr = (char*)realloc(mem->memory, mem->size + realsize + 1);
  if(!ptr) {
    /* out of memory! */
    printf("not enough memory (realloc returned NULL)\n");
    return 0;
  }

  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

static std::string CallApi(char* title)
{
    string reqTitle = std::string(title);
    CURL *curl = curl_easy_init();
    struct MemoryStruct chunk;

    if(curl)
    {
        chunk.memory = (char*)malloc(1);  /* will be grown as needed by the realloc above */
        chunk.size = 0;    /* no data at this point */
        curl_easy_setopt(curl, CURLOPT_URL, "http://baobianapi.pullword.com:9091/get.php");

        /* size of the POST data */
        int size = reqTitle.size();
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, size);

        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, reqTitle.c_str());
        curl_easy_setopt(curl,CURLOPT_ACCEPT_ENCODING,"");
        /* send all data to this function  */
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);

        /* we pass our 'chunk' struct to the callback function */
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_perform(curl);
    }

    curl_easy_cleanup(curl);

    return std::string(chunk.memory);
}

static double CalculateTitle(char* title)
{
    std::string result = CallApi(title);
    auto jData = json::parse(result);

    double ret = 0.0;
    if (jData.find("result") != jData.end()) 
    {
       ret = jData.at("result");
    }
    return ret;
    
}

bool IsLessThan(double a,double b)
{
    return a < b;
}

static json GetNegNewsInfo()
{
    CURL *conn = NULL;
    CURLcode code;
    vector<TitleUrl> arr;
    
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::string url = "http://news.baidu.com/";
    if (!init(conn, const_cast<char*>(url.c_str())))
    {
        fprintf(stderr, "Connection initializion failed/n");
        exit(EXIT_FAILURE);
    }
    code = curl_easy_perform(conn);
    curl_easy_cleanup(conn);
    if (code != CURLE_OK)
    {
        fprintf(stderr, "Failed to get '%s' [%s]/n", const_cast<char*>(url.c_str()), errorBuffer);
        exit(EXIT_FAILURE);
    }

    ParseHtml(buffer, arr);

    int arr_size = arr.size();

    json rootValue;
    for(int i = 0; i < arr_size; i ++)
    {
        if(arr[i].title.empty())
        {
            continue;
        }
        else
        {
            if(IsLessThan(CalculateTitle(const_cast<char*>(arr[i].title.c_str())),-0.5))
            {
                rootValue.emplace(arr[i].title, arr[i].url);
            }

        }
    }

    return rootValue;
}
