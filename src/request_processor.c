/*
 * =====================================================================================
 *
 *       Filename:  request_processor.c
 *
 *    Description:
 *
 *        Version:  1.0
 *        Created:  07/20/2016 14:40:14
 *       Revision:  none
 *       Copyright: Copyright (c) 2016, GoodCitizen Co. Ltd.
 *
 *         Author:  Samuel Sekiwere (SS), sekiskylink@gmail.com
 *   Organization:
 *
 * =====================================================================================
 */
#include <gwlib/gwlib.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <libpq-fe.h>
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <jansson.h>

#include "request_processor.h"

static dispatcher2conf_t dispatcher2conf;
static List *srvlist;

static xmlChar *findvalue(xmlDocPtr doc, xmlChar *xpath, int add_namespace){
    xmlNodeSetPtr nodeset;
    xmlChar *value = NULL;
    xmlXPathContextPtr context;
    xmlXPathObjectPtr result;
    int i;

    if(!doc){
        info(0, "Null Doc for %s", xpath);
        return NULL;
    }
    context = xmlXPathNewContext(doc);
    if(!context){
        info(0, "Null context for %s", xpath);
        return NULL;
    }

    if (add_namespace){
        xmlChar *prefix = (xmlChar *) "xmlns";
        xmlChar *href = (xmlChar *) "http://dhis2.org/schema/dxf/2.0";
        if (xmlXPathRegisterNs(context, prefix, href) != 0){
            info(0, "Unable to register NS:%s", prefix);
            return NULL;
        }
    }
    result = xmlXPathEvalExpression(xpath, context);
    if(!result){
        info(0, "Null result for %s", xpath);
        return NULL;
    }
    if(xmlXPathNodeSetIsEmpty(result->nodesetval)){
        xmlXPathFreeObject(result);
        info(0, "xmlXPathNodeSetIsEmpty for %s", xpath);
        return NULL;
    }
    nodeset = result->nodesetval;
    /* We return first match */
    for (i=0; i < nodeset->nodeNr; i++) {
        value = xmlNodeListGetString(doc, nodeset->nodeTab[i]->xmlChildrenNode, 1);
        if(value)
            break;
    }
    xmlXPathFreeContext(context);
    xmlXPathFreeObject(result);

    return value;
}

#define REQUEST_SQL "SELECT id FROM requests WHERE status = 'ready' AND is_allowed_source(source, destination) ORDER BY created ASC LIMIT 10000"

static void init_request_processor_sql(PGconn *c)
{
    PGresult *r;
    if (PQstatus(c) != CONNECTION_OK)
        return;

    r = PQprepare(c, "REQUEST_SQL", REQUEST_SQL, 0, NULL);
    PQclear(r);
}

static List *req_list; /* Of Request id */
static Dict *req_dict; /* For keeping list short*/
static Dict *server_dict;

void free_serverconf(serverconf_t *d)
{
    if (!d)
        return;
    octstr_destroy(d->name);
    octstr_destroy(d->username);
    octstr_destroy(d->password);
    octstr_destroy(d->ipaddress);
    octstr_destroy(d->url);
    octstr_destroy(d->auth_method);
    octstr_destroy(d->http_method);
    octstr_destroy(d->ssl_client_certkey_file);
    gw_free(d);
}

static void load_serverconf_dict(PGconn *c)
{
    char buf[128], *s;
    PGresult *r;
    int i, n;
    if (!c)
        return;
    server_dict = dict_create(17, (void *)free_serverconf);

    sprintf(buf, "SELECT * FROM servers");

    r = PQexec(c, buf);
    n = (PQresultStatus(r) == PGRES_TUPLES_OK) ? PQntuples(r) : 0;
    for (i=0; i<n; i++) {
        info(0, "We got here XXX!");
        serverconf_t *server = gw_malloc(sizeof *server);
        server->server_id = (s = PQgetvalue(r, i, PQfnumber(r, "id"))) != NULL ? strtoul(s, NULL, 10) : 0;
        Octstr *xkey = octstr_format("%s", s);

        server->name = octstr_create(PQgetvalue(r, i, PQfnumber(r, "name")));
        server->username = octstr_create(PQgetvalue(r, i, PQfnumber(r, "username")));
        server->password = octstr_create(PQgetvalue(r, i, PQfnumber(r, "password")));
        server->ipaddress = octstr_create(PQgetvalue(r, i, PQfnumber(r, "ipaddress")));
        server->url = octstr_create(PQgetvalue(r, i, PQfnumber(r, "url")));
        server->auth_method = octstr_create(PQgetvalue(r, i, PQfnumber(r, "auth_method")));
        server->http_method = octstr_create(PQgetvalue(r, i, PQfnumber(r, "http_method")));
        server->ssl_client_certkey_file = octstr_create(PQgetvalue(r, i, PQfnumber(r, "ssl_client_certkey_file")));
        server->use_ssl = strcmp(PQgetvalue(r, i, PQfnumber(r, "use_ssl")), "t") == 0 ?  1 : 0;
        server->parse_responses = strcmp(PQgetvalue(r, i, PQfnumber(r, "parse_responses")), "t") == 0 ?  1 : 0;
        server->start_submission_period = (s = PQgetvalue(r, i,
                    PQfnumber(r, "start_submission_period"))) != NULL ? strtoul(s, NULL, 10) : 0;
        server->end_submission_period = (s = PQgetvalue(r, i,
                    PQfnumber(r, "end_submission_period"))) != NULL ? strtoul(s, NULL, 10) : 0;

        dict_put(server_dict, xkey, server);
        octstr_destroy(xkey);
    }
    PQclear(r);
    return;
}

/* Post XML to server using basic auth and return response */
static Octstr *post_payload_to_server(Octstr *data, Octstr *ctype,
        serverconf_t *dest, int body_is_query_param) {
    HTTPCaller *caller;

    List *request_headers;
    Octstr *furl = NULL, *rbody = NULL, *xurl = NULL;
    int method = HTTP_METHOD_POST; /* default is POST */
    int i, status = -1;

    if (octstr_compare(dest->http_method, octstr_imm("GET")) == 0)
        method = HTTP_METHOD_GET;

    /*  request_headers = http_create_empty_headers();*/
    request_headers = gwlist_create();
    if (ctype && octstr_case_search(ctype, octstr_imm("json"), 0) >= 0)
        http_header_add(request_headers, "Content-Type", "application/json");
    else
        http_header_add(request_headers, "Content-Type", "application/xml");

    http_add_basic_auth(request_headers, dest->username, dest->password);
    /* XXX Add SSL stuff here */
    caller = http_caller_create();
    if (dest->use_ssl && (octstr_compare(dest->ssl_client_certkey_file, octstr_imm("")) != 0)){
        info(0, "Using HTTPS client to post data: certkey_file:%s!",
                octstr_get_cstr(dest->ssl_client_certkey_file));
        if (body_is_query_param == 0) {
            http_start_request(caller, method, dest->url, request_headers, data, 1, NULL,
                dest->ssl_client_certkey_file);
        } else {
            /* append body to url nicely and make call */
            if ((i = octstr_search_char(dest->url, '?', 0)) > 0) {
                xurl = octstr_format("%S%S", dest->url, data);
            } else{
                xurl = octstr_format("%S?%S", dest->url, data);
            }
            http_start_request(caller, method, xurl, request_headers, NULL, 1, NULL,
                    dest->ssl_client_certkey_file);
        }
    } else {
        info(0, "Using normal HTTP client to post data!");
        if (body_is_query_param == 0) {
            http_start_request(caller, method, dest->url, request_headers, data, 1, NULL, NULL);
        } else {
            /* append body to url nicely and make call */
            if ((i = octstr_search_char(dest->url, '?', 0)) > 0) {
                xurl = octstr_format("%S%S", dest->url, data);
            } else{
                xurl = octstr_format("%S?%S", dest->url, data);
            }
            http_start_request(caller, method, xurl, request_headers, NULL, 1, NULL, NULL);
        }
    }
    http_receive_result_real(caller, &status, &furl, &request_headers, &rbody, 1);

    http_caller_destroy(caller);
    http_destroy_headers(request_headers);
    octstr_destroy(furl);
    octstr_destroy(xurl);
    /*  octstr_destroy(rbody); */

    if (status == -1){
        if(rbody) octstr_destroy(rbody);
        return NULL;
    }
    return rbody;
}

static void do_request(PGconn *c, int64_t rid) {
    char tmp[64] = {0}, *cmd, *x, buf[256] = {0}, st[64] = {0};
    PGresult *r;
    int retries, serverid, source, body_is_query_param = 0;
    Octstr *data;
    Octstr *ctype;
    const char *pvals[] = {tmp, st, buf};
    Octstr *resp, *xkey;
    xmlDocPtr doc;
    xmlChar *s, *im, *ig, *up;
    json_t *root, *status, *descr;
    json_error_t error;
    const char *status_text, *description;

    sprintf(tmp, "%ld", rid);

    /* NOWAIT forces failure if the record is locked. Also we do not process if not yet time.*/
    cmd = "SELECT source, destination, body, retries, in_submission_period(destination), ctype, "
        " body_is_query_param FROM requests WHERE id = $1 FOR UPDATE NOWAIT";

    r = PQexecParams(c, cmd, 1, NULL, pvals, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) <= 0) {
        /*skip this one*/
        PQclear(r);
        return;
    }

    if ((x = PQgetvalue(r, 0, 4)) && (strcmp(x, "f") == 0)) {
        /* We're out of submission period */
        info(0, "Destination Server Out of Submission Period");
        return;
    }
    source = (x = PQgetvalue(r, 0, 0)) != NULL ? atoi(x) : -1;
    serverid = (x = PQgetvalue(r, 0, 1)) != NULL ? atoi(x) : -1;
    retries = (x = PQgetvalue(r, 0, 3)) != NULL ? atoi(x) : -1;
    x = PQgetvalue(r, 0, 5);
    ctype = (x && x[0]) ? octstr_create(x): NULL;
    x = PQgetvalue(r, 0, 2);
    data = (x && x[0]) ? octstr_create(x) : NULL; /* POST XML or JSON */
    if ((x = PQgetvalue(r, 0, 6)) && (strcmp(x, "t") == 0))
        body_is_query_param = 1;


    PQclear(r);
    info(0, "Post Data %s", octstr_get_cstr(data));

    if (retries > dispatcher2conf->max_retries) {
        r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "status = 'expired' WHERE id = $1",
                1, NULL, pvals, NULL, NULL, 0);
        PQclear(r);
        return;
    }

    if (!data){
        r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "statuscode='ERROR1', errors = 'Empty response from server', "
                "status = 'failed' WHERE id = $1",
                1, NULL, pvals, NULL, NULL, 0);
        PQclear(r);
        /* Mark this one as failed*/
        return;
    }

    xkey = octstr_format("%d", serverid);
    serverconf_t *dest = dict_get(server_dict, xkey);
    if (!dest){
        info(0, "Failed to get server conf for server: %d", serverid);
        return;
    }

    resp = post_payload_to_server(data, ctype, dest, body_is_query_param);

    if (!resp) {
        r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "statuscode = 'ERROR2', errors = 'Server possibly unreachable!', "
                "status = 'failed' WHERE id = $1",
                1, NULL, pvals, NULL, NULL, 0);
        PQclear(r);
        return;
    }
    info(0, "Response Data %s", octstr_get_cstr(resp));
    if (!dest->parse_responses){
        r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "statuscode = 'SUCCESS', status = 'completed' WHERE id = $1",
                1, NULL, pvals, NULL, NULL, 0);
        PQclear(r);
        return;
    }

    if (ctype && octstr_case_search(ctype, octstr_imm("xml"), 0) >= 0) {
        /* parse response - hopefully it is xml */
        doc = xmlParseMemory(octstr_get_cstr(resp), octstr_len(resp));

        if (!doc) {
            r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                    "statuscode = 'ERROR3', errors = 'Response possibly not proper XML', "
                    "status = 'failed' WHERE id = $1",
                    1, NULL, pvals, NULL, NULL, 0);
            PQclear(r);
            return;
        }

        s = findvalue(doc, (xmlChar *)"//xmlns:status", 1); /* third arg is 0 if no namespace required*/
        im = findvalue(doc, (xmlChar *)"//xmlns:importCount[1]/@imported", 1);
        ig = findvalue(doc, (xmlChar *)"//xmlns:importCount[1]/@ignored", 1);
        up = findvalue(doc, (xmlChar *)"//xmlns:importCount[1]/@updated", 1);

        sprintf(st, "%s", s);
        sprintf(buf, "Imported:%s Ignored:%s Updated:%s",im, ig, up);
        if(s) xmlFree(s);
        if(im) xmlFree(im);
        if(ig) xmlFree(ig);
        if(up) xmlFree(up);

        if (strcasestr(st, "ERROR")) {
            r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "statuscode=$2, status = 'failed', errors = $3 WHERE id = $1",
                3, NULL, pvals, NULL, NULL, 0);
        } else {
            r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "statuscode=$2, status = 'completed', statuscode='SUCCESS', errors = $3 WHERE id = $1",
                3, NULL, pvals, NULL, NULL, 0);

        }
        PQclear(r);

        if(doc)
            xmlFreeDoc(doc);
    } else if (ctype && octstr_case_search(ctype, octstr_imm("json"), 0) >= 0) {
        /* Let's parse the JSON response */
        root = json_loads(octstr_get_cstr(resp), 0, &error);
        if (!root) {
            r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "statuscode = 'ERROR4', errors = 'Response was not proper JSON', "
                "status = 'failed' WHERE id = $1",
                1, NULL, pvals, NULL, NULL, 0);
            PQclear(r);
            goto done;
        }
        status = json_object_get(root, "status");
        if (!json_is_string(status)) {
            info(0, "Failed to parse JSON reposne: (status).");
            r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "statuscode = 'ERROR5', errors= 'Could not pick status from JSON response',"
                "status = 'failed' WHERE id = $1",
                1, NULL, pvals, NULL, NULL, 0);
            PQclear(r);
            json_decref(root);
            goto done;
        }
        status_text = json_string_value(status);
        sprintf(st, "%s", status_text);

        descr = json_object_get(root, "description");
        if (!json_is_string(descr)) {
            info(0, "Failed to parse JSON reposne: (description).");
            r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "statuscode = 'ERROR6', errors = 'No description field in JSON response',"
                "status = 'failed' WHERE id = $1",
                1, NULL, pvals, NULL, NULL, 0);
            PQclear(r);
            json_decref(root);
            goto done;
        }
        description = json_string_value(descr);
        sprintf(buf, "%s", description);

        if (strcasecmp(status_text, "ERROR") == 0) {
            r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "statuscode=$2, status = 'failed', errors = $3 WHERE id = $1",
                3, NULL, pvals, NULL, NULL, 0);
        } else {
            r = PQexecParams(c, "UPDATE requests SET updated = timeofday()::timestamp, "
                "statuscode=$2, status = 'completed', errors = $3 WHERE id = $1",
                3, NULL, pvals, NULL, NULL, 0);

        }
        PQclear(r);

    }
done:
    octstr_destroy(resp);
    octstr_destroy(xkey);
}

static void request_run(PGconn *c) {
    int64_t *rid;
    dispatcher2conf_t config = dispatcher2conf;

    if (srvlist != NULL)
        gwlist_add_producer(srvlist);
    while((rid = gwlist_consume(req_list)) != NULL) {
        PGresult *r;
        char tmp[64];
        Octstr *xkey;
        int64_t xid = *rid;

        time_t t = time(NULL);
        struct tm tm = gw_localtime(t);

        if (!(tm.tm_hour >= config->start_submission_period
                    && tm.tm_hour <= config->end_submission_period)){
            /* warning(0, "We're out of submission period"); */
            gwthread_sleep(config->request_process_interval);
            continue; /* we're outide submission period so stay silent*/
        }

        /* heal connection if bad*/
        r = PQexec(c, "BEGIN");
        PQclear(r);

        sprintf(tmp, "%ld", xid);
        xkey = octstr_format("Request-%s", tmp);
        info(0, "Gonna call do_request");
        do_request(c, xid);

        r = PQexec(c, "COMMIT");
        PQclear(r);

        gw_free(rid);
        dict_remove(req_dict, xkey);
        octstr_destroy(xkey);
    }
    PQfinish(c);
    if (srvlist != NULL)
        gwlist_remove_producer(srvlist);

}

static int qstop = 0;

#define HIGH_WATER_MARK 100000
#define MAX_QLEN 100000
static void run_request_processor(PGconn *c)
{
    /* Start worker threads
     * Produce jobs in the server_req_list
     * */
    int i, num_threads;
    char port_str[32];
    dispatcher2conf_t config = dispatcher2conf;

    sprintf(port_str, "%d", config->dbport);

    info(0, "Request processor starting up...");

    req_dict = dict_create(config->num_threads * MAX_QLEN + 1, NULL);
    req_list = gwlist_create();
    gwlist_add_producer(req_list);

    for (i = num_threads = 0; i<config->num_threads; i++) {
        PGconn *conn = PQsetdbLogin(config->dbhost, config->dbport > 0 ? port_str : NULL, NULL, NULL,
                    config->dbname, config->dbuser, config->dbpass);

        if (PQstatus(conn) != CONNECTION_OK) {
            error(0, "request_processor: Failed to connect to database: %s",
		     PQerrorMessage(conn));
	        PQfinish(conn);
        } else {
            gwthread_create((void *)request_run, conn);
            num_threads++;
        }
    }

    if (num_threads == 0)
        goto finish;

    do {
        PGresult *r;
        long i, n;
        time_t t = time(NULL);
        struct tm tm = gw_localtime(t);

        if (!(tm.tm_hour >= config->start_submission_period
                    && tm.tm_hour <= config->end_submission_period)){
            info(0, "We're out of submission period CURRENT HOUR:%d, Boundary[%d:%d]",
                    tm.tm_hour, config->start_submission_period, config->end_submission_period);
            gwthread_sleep(config->request_process_interval);
            continue; /* we're outide submission period so stay silent*/
        }

        gwthread_sleep(config->request_process_interval);
        if ((n = gwlist_len(req_list)) > 0)
            info(0, "We got here ###############%ld\n", gwlist_len(req_list));

        if (qstop)
            break;
        else if ((n = gwlist_len(req_list)) > HIGH_WATER_MARK) {
            warning(0, "Request processor: Too many (%ld) pending batches, will wait a little", n);
            continue;
        }

        if (PQstatus(c) != CONNECTION_OK) {
            /* Die...for real*/
            panic(0, "Request processor: Failed to connect to database: %s",
                    PQerrorMessage(c));
            PQfinish(c);

            c = PQsetdbLogin(config->dbhost, config->dbport > 0 ? port_str : NULL, NULL, NULL,
                    config->dbname, config->dbuser, config->dbpass);
            init_request_processor_sql(c);
            goto loop;
        }
        /*XXX lets populate server_dict here
        load_serverconf_dict(c);
        */

        r = PQexecPrepared(c, "REQUEST_SQL", 0, NULL, NULL, NULL, 0);
        n = PQresultStatus(r) == PGRES_TUPLES_OK ? PQntuples(r) : 0;
        if (n > 0)
            info(0, "Got %ld Ready requests to add to request-list", n);
        for (i=0; i<n; i++) {
            char *y = PQgetvalue(r, i, 0);
            Octstr *xkey = octstr_format("Request-%s", y);
            if (dict_put_once(req_dict, xkey, (void*)1) == 1) { /* Item not in queue waiting*/
                int64_t rid;
                rid = y && isdigit(y[0]) ? strtoul(y, NULL, 10) : 0;

                int64_t *x = gw_malloc(sizeof *x);
                *x = rid;

                gwlist_produce(req_list, x);
                octstr_destroy(xkey);
            }
        }
        PQclear(r);
    loop:
        (void)0;
    } while (qstop == 0);

finish:
    PQfinish(c);
    gwlist_remove_producer(req_list);
    gwthread_join_every((void *)request_run);
    gwlist_destroy(req_list, NULL);
    info(0, "Request processor exited!!!");
    dict_destroy(req_dict);
}

static long rthread_th = -1;
void start_request_processor(dispatcher2conf_t config, List *server_req_list)
{
    PGconn *c;
    char port_str[32];
    dispatcher2conf = config;

    sprintf(port_str, "%d", config->dbport);

    c = PQsetdbLogin(config->dbhost, config->dbport > 0 ? port_str : NULL, NULL, NULL,
            config->dbname, config->dbuser, config->dbpass);
    if (PQstatus(c) != CONNECTION_OK) {
        error(0, "Request processor: Failed to connect to database: %s",
		PQerrorMessage(c));
        return;
    }
    load_serverconf_dict(c);

    init_request_processor_sql(c);

    srvlist = server_req_list;
    rthread_th = gwthread_create((void *) run_request_processor, c);
}

void stop_request_processor(void)
{

     qstop = 1;
     dict_destroy(server_dict);
     gwthread_sleep(2); /* Give them some time */
     gwthread_wakeup(rthread_th);

     gwthread_sleep(2); /* Give them some time */
     gwthread_join(rthread_th);

     info(0, "Request processor shutdown complete");
}
