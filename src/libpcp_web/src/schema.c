/*
 * Copyright (c) 2017-2018 Red Hat.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */
#include <assert.h>
#include "pmapi.h"
#include "pmda.h"
#include "schema.h"
#include "discover.h"
#include "util.h"
#include "sha1.h"

#define STRINGIFY(s)	#s
#define TO_STRING(s)	STRINGIFY(s)
#define SCHEMA_VERSION	2
#define SHA1SZ		20

typedef struct redisScript {
    sds			hash;
    const char		*text;
} redisScript;

static redisScript	*scripts;
static int		nscripts;

static void
redisScriptsInit(void)
{
    const unsigned char	*text;
    unsigned char	hash[20];
    char		hashbuf[42];
    redisScript		*script;
    SHA1_CTX		shactx;
    int			i;

    for (i = 0; i < nscripts; i++) {
	script = &scripts[i];
	text = (const unsigned char *)script->text;

	/* Calculate unique script identifier from its contents */
	SHA1Init(&shactx);
	SHA1Update(&shactx, text, strlen((char *)text));
	SHA1Final(hash, &shactx);
	pmwebapi_hash_str(hash, hashbuf, sizeof(hashbuf));
	scripts->hash = sdsnew(hashbuf);
    }
}

static int
testReplyError(redisReply *reply, const char *server_message)
{
    return (reply && reply->type == REDIS_REPLY_ERROR &&
	    strcmp(reply->str, server_message) == 0);
}

static void
reportReplyError(redisInfoCallBack info, void *userdata,
	redisReply *reply, const char *format, va_list argp)
{
    sds			msg = sdsnew("Error: ");

    msg = sdscatvprintf(msg, format, argp);
    if (reply && reply->type == REDIS_REPLY_ERROR)
	msg = sdscatfmt(msg, "\nRedis: %s\n", reply->str);
    else
	msg = sdscat(msg, "\n");
    info(PMLOG_RESPONSE, msg, userdata);
    sdsfree(msg);
}

static int
checkStatusReplyOK(redisInfoCallBack info, void *userdata,
		redisReply *reply, const char *format, ...)
{
    va_list		argp;

    if (reply->type == REDIS_REPLY_STATUS &&
	(strcmp("OK", reply->str) == 0 || strcmp("QUEUED", reply->str) == 0))
	return 0;
    va_start(argp, format);
    reportReplyError(info, userdata, reply, format, argp);
    va_end(argp);
    return -1;
}

static int
checkStreamReplyString(redisInfoCallBack info, void *userdata,
	redisReply *reply, sds s, const char *format, ...)
{
    va_list		argp;

    if (reply->type == REDIS_REPLY_STRING && strcmp(s, reply->str) == 0)
	return 0;
    va_start(argp, format);
    reportReplyError(info, userdata, reply, format, argp);
    va_end(argp);
    return -1;
}

static int
checkArrayReply(redisInfoCallBack info, void *userdata,
	redisReply *reply, const char *format, ...)
{
    va_list		argp;

    if (reply && reply->type == REDIS_REPLY_ARRAY)
	return 0;
    va_start(argp, format);
    reportReplyError(info, userdata, reply, format, argp);
    va_end(argp);
    return -1;
}

static long long
checkIntegerReply(redisInfoCallBack info, void *userdata,
	redisReply *reply, const char *format, ...)
{
    va_list		argp;

    if (reply && reply->type == REDIS_REPLY_INTEGER)
	return reply->integer;
    va_start(argp, format);
    reportReplyError(info, userdata, reply, format, argp);
    va_end(argp);
    return -1;
}

static sds
checkStringReply(redisInfoCallBack info, void *userdata,
	redisReply *reply, const char *format, ...)
{
    va_list		argp;

    if (reply && reply->type == REDIS_REPLY_STRING)
	return sdsnew(reply->str);
    va_start(argp, format);
    reportReplyError(info, userdata, reply, format, argp);
    va_end(argp);
    return NULL;
}

static void
initRedisMapBaton(redisMapBaton *baton, redisSlots *slots,
	redisMap *mapping, sds mapKey, sds mapStr,
	redisDoneCallBack on_done, redisInfoCallBack on_info,
	void *userdata, void *arg)
{
    initSeriesBatonMagic(baton, MAGIC_MAPPING);
    baton->mapping = mapping;
    baton->mapKey = mapKey;
    baton->mapStr = mapStr;
    baton->slots = slots;
    baton->info = on_info;
    baton->mapped = on_done;
    baton->userdata = userdata;
    baton->arg = arg;
}

static void
doneRedisMapBaton(redisMapBaton *baton)
{
    seriesBatonCheckMagic(baton, MAGIC_MAPPING, "doneRedisMapBaton");
    if (baton->mapped)
	baton->mapped(baton->arg);
    memset(baton, 0, sizeof(*baton));
    free(baton);
}

static void
redis_map_publish_callback(redisAsyncContext *redis, redisReply *reply, void *arg)
{
    redisMapBaton	*baton = (redisMapBaton *)arg;
    const char		*mapname;

    seriesBatonCheckMagic(baton, MAGIC_MAPPING, "redis_map_publish_callback");
    mapname = redisMapName(baton->mapping);
    checkIntegerReply(baton->info, baton->userdata, reply,
		    "%s: %s", PUBLISH, "new %s mapping", mapname);
    doneRedisMapBaton(baton);
}

static void
redis_map_request_callback(redisAsyncContext *redis, redisReply *reply, void *arg)
{
    redisMapBaton	*baton = (redisMapBaton *)arg;
    redisSlots		*slots = (redisSlots *)baton->slots;
    const char		*mapname;
    sds			cmd, msg, key;
    int			newname;

    seriesBatonCheckMagic(baton, MAGIC_MAPPING, "redis_map_request_callback");

    mapname = redisMapName(baton->mapping);
    newname = checkIntegerReply(baton->info, baton->userdata, reply,
			"%s: %s (%s)", HSET, "string mapping script", mapname);

    if (newname <= 0) {
	doneRedisMapBaton(baton);
    } else {
	/* publish any newly created name mapping */
	msg = sdscatfmt(sdsempty(), "%S:%S", baton->mapKey, baton->mapStr);
	key = sdscatfmt(sdsempty(), "pcp:channel:%s", mapname);
	cmd = redis_command(3);
	cmd = redis_param_str(cmd, PUBLISH, PUBLISH_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sds(cmd, msg);
	sdsfree(msg);

	redisSlotsRequest(slots, PUBLISH, key, cmd, redis_map_publish_callback, baton);
    }
}

void
redisMapRequest(redisMapBaton *baton, redisMap *map, sds name, sds value)
{
    sds			cmd, key;

    key = sdscatfmt(sdsempty(), "pcp:map:%s", redisMapName(baton->mapping));
    cmd = redis_command(4);
    cmd = redis_param_str(cmd, HSET, HSET_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sds(cmd, name);
    cmd = redis_param_sds(cmd, value);

    redisSlotsRequest(baton->slots, HSET, key, cmd, redis_map_request_callback, baton);
}

void
redisGetMap(redisSlots *slots, redisMap *mapping, unsigned char *hash, sds mapStr,
		redisDoneCallBack on_done, redisInfoCallBack on_info,
		void *userdata, void *arg)
{
    redisMapBaton	*baton;
    redisMapEntry	*entry;
    sds			mapKey;

    pmwebapi_string_hash(hash, mapStr, sdslen(mapStr));
    mapKey = sdsnewlen(hash, 20);

    if ((entry = redisMapLookup(mapping, mapKey)) != NULL) {
	sdsfree(mapKey);
	on_done(arg);
    } else {
	/*
	 * This string is not cached locally; so we always send it to server;
	 * it may or may not exist there yet, we must just make sure it does.
	 * The caller does not need to wait as we provide the calculated hash
	 * straight away.
	 */
	if ((baton = calloc(1, sizeof(redisMapBaton))) != NULL) {
	    initRedisMapBaton(baton, slots, mapping, mapKey, mapStr,
			    on_done, on_info, userdata, arg);
	    redisMapInsert(mapping, mapKey, mapStr);
	    redisMapRequest(baton, mapping, mapKey, mapStr);
	} else {
	    on_done(arg);
	}
    }
}

static void
redis_source_context_name(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;

    checkIntegerReply(baton->info, baton->userdata,
		reply, "%s: %s", SADD, "mapping context to source or host name");
    doneSeriesLoadBaton(baton, "redis_source_context_name");
}

static void
redis_source_location(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;

    checkIntegerReply(baton->info, baton->userdata,
		reply, "%s: %s", GEOADD, "mapping source location");
    doneSeriesLoadBaton(baton, "redis_source_location");
}

static void
redis_context_name_source(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;

    checkIntegerReply(baton->info, baton->userdata,
		reply, "%s: %s", SADD, "mapping source names to context");
    doneSeriesLoadBaton(baton, "redis_context_name_source");
}

void
redis_series_source(redisSlots *slots, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    context_t			*context = seriesLoadBatonContext(baton);
    char			hashbuf[42];
    sds				cmd, key, val, val2;

    /* Async recipe:
     * . SADD pcp:source:context.name:<id>
     * . SADD pcp:context.name:source:<hash>
     * . SADD pcp:source:context.name:<hostid>
     * . GEOADD pcp:source:location <lat> <long> <hash>
     */
    seriesBatonReferences(baton, 4, "redis_series_source");

    pmwebapi_hash_str(context->name.id, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "pcp:source:context.name:%s", hashbuf);
    cmd = redis_command(3);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sha(cmd, context->name.hash);
    redisSlotsRequest(slots, SADD, key, cmd, redis_source_context_name, arg);

    pmwebapi_hash_str(context->hostid, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "pcp:source:context.name:%s", hashbuf);
    cmd = redis_command(3);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sha(cmd, context->name.hash);
    redisSlotsRequest(slots, SADD, key, cmd, redis_source_context_name, arg);

    pmwebapi_hash_str(context->name.hash, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "pcp:context.name:source:%s", hashbuf);
    cmd = redis_command(4);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sha(cmd, context->name.id);
    cmd = redis_param_sha(cmd, context->hostid);
    redisSlotsRequest(slots, SADD, key, cmd, redis_context_name_source, arg);

    key = sdsnew("pcp:source:location");
    val = sdscatprintf(sdsempty(), "%.8f", context->location[0]);
    val2 = sdscatprintf(sdsempty(), "%.8f", context->location[1]);
    cmd = redis_command(5);
    cmd = redis_param_str(cmd, GEOADD, GEOADD_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sds(cmd, val2);
    cmd = redis_param_sds(cmd, val);
    cmd = redis_param_sha(cmd, context->name.hash);
    sdsfree(val2);
    sdsfree(val);
    redisSlotsRequest(slots, GEOADD, key, cmd, redis_source_location, arg);
}

static void
redis_series_inst_name_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;

    checkIntegerReply(baton->info, baton->userdata,
		reply, "%s: %s", SADD, "mapping series to inst name");
    doneSeriesLoadBaton(baton, "redis_series_inst_name_callback");
}

static void
redis_instances_series_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;

    checkIntegerReply(baton->info, baton->userdata,
		reply, "%s: %s", SADD, "mapping instance to series");
    doneSeriesLoadBaton(baton, "redis_instances_series_callback");
}

static void
redis_series_inst_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;

    checkStatusReplyOK(baton->info, baton->userdata,
		reply, "%s: %s", HMSET, "setting metric inst");
    doneSeriesLoadBaton(baton, "redis_series_inst_callback");
}

void
redis_series_instance(redisSlots *slots, metric_t *metric, instance_t *instance, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    char			hashbuf[42];
    sds				cmd, key, val;
    int				i;

    seriesBatonCheckMagic(baton, MAGIC_LOAD, "redis_series_instance");
    seriesBatonReferences(baton, 2, "redis_series_instance");

    assert(instance->name.sds);

    pmwebapi_hash_str(instance->name.id, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "pcp:series:inst.name:%s", hashbuf);
    cmd = redis_command(2 + metric->numnames);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    for (i = 0; i < metric->numnames; i++)
	cmd = redis_param_sha(cmd, metric->names[i].hash);
    redisSlotsRequest(slots, SADD, key, cmd, redis_series_inst_name_callback, arg);

    for (i = 0; i < metric->numnames; i++) {
	seriesBatonReference(baton, "redis_series_instance");
	pmwebapi_hash_str(metric->names[i].hash, hashbuf, sizeof(hashbuf));
	key = sdscatfmt(sdsempty(), "pcp:instances:series:%s", hashbuf);
	cmd = redis_command(3);
	cmd = redis_param_str(cmd, SADD, SADD_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sha(cmd, instance->name.hash);
	redisSlotsRequest(slots, SADD, key, cmd, redis_instances_series_callback, arg);
    }

    pmwebapi_hash_str(instance->name.hash, hashbuf, sizeof(hashbuf));
    val = sdscatfmt(sdsempty(), "%i", instance->inst);
    key = sdscatfmt(sdsempty(), "pcp:inst:series:%s", hashbuf);
    cmd = redis_command(8);
    cmd = redis_param_str(cmd, HMSET, HMSET_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_str(cmd, "inst", sizeof("inst")-1);
    cmd = redis_param_sds(cmd, val);
    cmd = redis_param_str(cmd, "name", sizeof("name")-1);
    cmd = redis_param_sha(cmd, instance->name.id);
    cmd = redis_param_str(cmd, "source", sizeof("series")-1);
    cmd = redis_param_sha(cmd, metric->indom->domain->context->name.hash);
    sdsfree(val);
    redisSlotsRequest(slots, HMSET, key, cmd, redis_series_inst_callback, arg);
}

static void
label_value_mapping_callback(void *arg)
{
    labellist_t			*list = (labellist_t *)arg;
    seriesLoadBaton		*baton = (seriesLoadBaton *)list->arg;

    seriesBatonCheckMagic(baton, MAGIC_LOAD, "label_value_mapping_callback");
    redisMapRelease(list->valuemap);
    list->valuemap = NULL;
    doneSeriesLoadBaton(baton, "label_value_mapping_callback");
}

static void
label_name_mapping_callback(void *arg)
{
    labellist_t			*list = (labellist_t *)arg;
    seriesLoadBaton		*baton = (seriesLoadBaton *)list->arg;

    seriesBatonCheckMagic(baton, MAGIC_LOAD, "label_name_mapping_callback");
    doneSeriesLoadBaton(baton, "label_name_mapping_callback");
}

typedef struct seriesAnnotateClosure {
    struct seriesLoadBaton	*load;
    metric_t			*metric;
    instance_t			*instance;
} seriesAnnotateClosure;

static int
annotate_metric(const pmLabel *label, const char *json, void *arg)
{
    seriesAnnotateClosure	*closure = (seriesAnnotateClosure *)arg;
    seriesLoadBaton		*baton = closure->load;
    labellist_t			*list;
    instance_t			*instance = closure->instance;
    metric_t			*metric = closure->metric;
    char			hashbuf[42];
    sds				key;

    seriesBatonCheckMagic(baton, MAGIC_LOAD, "annotate_metric");

    /* check if this label is already in the list */
    list = instance ? instance->labellist : metric->labellist;
    while (list) {
	if (label->namelen == sdslen(list->name) &&
	    strncmp(list->name, json + label->name, label->namelen) == 0)
	    return 0;	/* short-circuit */
	list = list->next;
    }

    /*
     * TODO: decode complex values ('{...}' and '[...]'),
     * using a dot-separated name for these maps, and names
     * with explicit array index suffix for array entries.
     */

    if ((list = (labellist_t *)calloc(1, sizeof(labellist_t))) == NULL)
	return -ENOMEM;

    list->arg = baton;
    list->name = sdsnewlen(json + label->name, label->namelen);
    list->value = sdsnewlen(json + label->value, label->valuelen);
    list->flags = label->flags;

    if (pmDebugOptions.series) {
	fprintf(stderr, "Annotate metric %s", metric->names[0].sds);
	if (instance)
	    fprintf(stderr, "[%s]", instance->name.sds);
	fprintf(stderr, " label %s=%s (flags=0x%x)\n",
			list->name, list->value, list->flags);
    }

    /* prepend map onto the list for this metric or instance */
    if (instance) {
	if (instance->labellist)
	    list->next = instance->labellist;
	instance->labellist = list;
    } else {
	if (metric->labellist)
	    list->next = metric->labellist;
	metric->labellist = list;
    }

    seriesBatonReferences(baton, 2, "annotate_metric");

    redisGetMap(baton->slots,
		labelsmap, list->nameid, list->name,
		label_name_mapping_callback,
		baton->info, baton->userdata, (void *)list);

    pmwebapi_hash_str(list->nameid, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "label.%s.value", hashbuf);
    list->valuemap = redisMapCreate(key);

    redisGetMap(baton->slots,
		list->valuemap, list->valueid, list->value,
		label_value_mapping_callback,
		baton->info, baton->userdata, (void *)list);

    return 0;
}

static void
redis_series_labelvalue_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;

    checkStatusReplyOK(load->info, load->userdata,
		reply, "%s: %s", HMSET, "setting series label value");
    doneSeriesLoadBaton(arg, "redis_series_labelvalue_callback");
}

static void
redis_series_labelflags_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;

    checkStatusReplyOK(load->info, load->userdata,
		reply, "%s: %s", HMSET, "setting series label flags");
    doneSeriesLoadBaton(arg, "redis_series_labelflags_callback");
}

static void
redis_series_label_set_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;

    checkIntegerReply(load->info, load->userdata,
		reply, "%s %s", SADD, "pcp:series:label.X.value:Y");
    doneSeriesLoadBaton(arg, "redis_series_label_set_callback");
}

static void
redis_series_label(redisSlots *slots, metric_t *metric, char *hash,
		labellist_t *list, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    char			namehash[42], valhash[42];
    sds				cmd, key, val;
    int				i;

    seriesBatonReferences(baton, 2, "redis_series_label");

    if (list->flags != PM_LABEL_CONTEXT) {
	seriesBatonReference(baton, "redis_series_label");

	val = sdscatfmt(sdsempty(), "%I", list->flags);
	key = sdscatfmt(sdsempty(), "pcp:labelflags:series:%s", hash);
	cmd = redis_command(4);
	cmd = redis_param_str(cmd, HMSET, HMSET_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sha(cmd, list->nameid);
	cmd = redis_param_sds(cmd, val);
	sdsfree(val);
	redisSlotsRequest(slots, HMSET, key, cmd,
				redis_series_labelflags_callback, arg);
    }

    key = sdscatfmt(sdsempty(), "pcp:labelvalue:series:%s", hash);
    cmd = redis_command(4);
    cmd = redis_param_str(cmd, HMSET, HMSET_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sha(cmd, list->nameid);
    cmd = redis_param_sha(cmd, list->valueid);
    redisSlotsRequest(slots, HMSET, key, cmd,
			redis_series_labelvalue_callback, arg);

    pmwebapi_hash_str(list->nameid, namehash, sizeof(namehash));
    pmwebapi_hash_str(list->valueid, valhash, sizeof(valhash));
    key = sdscatfmt(sdsempty(), "pcp:series:label.%s.value:%s",
		    namehash, valhash);
    cmd = redis_command(2 + metric->numnames);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    for (i = 0; i < metric->numnames; i++)
	cmd = redis_param_sha(cmd, metric->names[i].hash);
    redisSlotsRequest(slots, SADD, key, cmd,
				redis_series_label_set_callback, arg);
}

static void
redis_series_labelset(redisSlots *slots, metric_t *metric, instance_t *instance, void *arg)
{
    labellist_t			*list;
    char			hashbuf[42];
    int				i;

    if (instance != NULL) {
	pmwebapi_hash_str(instance->name.hash, hashbuf, sizeof(hashbuf));
	list = instance->labellist;
	do {
	    redis_series_label(slots, metric, hashbuf, list, arg);
	} while ((list = list->next) != NULL);
    } else {
	for (i = 0; i < metric->numnames; i++) {
	    pmwebapi_hash_str(metric->names[0].hash, hashbuf, sizeof(hashbuf));
	    list = metric->labellist;
	    do {
		redis_series_label(slots, metric, hashbuf, list, arg);
	    } while ((list = list->next) != NULL);
	}
    }
}

static void
series_label_mapping_fail(seriesname_t *series, int sts, seriesLoadBaton *baton)
{
    char			pmmsg[PM_MAXERRMSGLEN];
    char			hashbuf[42];
    sds				msg;

    pmwebapi_hash_str(series->hash, hashbuf, sizeof(hashbuf));
    infofmt(msg, "Cannot merge metric %s [%s] label set: %s", hashbuf,
		series->sds, pmErrStr_r(sts, pmmsg, sizeof(pmmsg)));
    batoninfo(baton, PMLOG_ERROR, msg);
}

void
series_metric_label_mapping(metric_t *metric, seriesLoadBaton *baton)
{
    seriesAnnotateClosure	closure = { baton, metric, NULL };
    char			buf[PM_MAXLABELJSONLEN];
    int				sts;

    if ((sts = metric_labelsets(metric, buf, sizeof(buf),
				    annotate_metric, &closure)) < 0)
	series_label_mapping_fail(&metric->names[0], sts, baton);
}

void
series_instance_label_mapping(metric_t *metric, instance_t *instance,
				seriesLoadBaton *baton)
{
    seriesAnnotateClosure	closure = { baton, metric, instance };
    char			buf[PM_MAXLABELJSONLEN];
    int				sts;

    if ((sts = instance_labelsets(metric->indom, instance, buf, sizeof(buf),
				  annotate_metric, &closure)) < 0)
	series_label_mapping_fail(&instance->name, sts, baton);
}

static void
series_name_mapping_callback(void *arg)
{
    seriesBatonCheckMagic(arg, MAGIC_LOAD, "series_name_mapping_callback");
    doneSeriesLoadBaton(arg, "series_name_mapping_callback");
}

static void redis_series_metadata(context_t *, metric_t *, void *);
static void redis_series_streamed(sds, metric_t *, void *);

void
redis_series_metric(redisSlots *slots, metric_t *metric,
		sds timestamp, int meta, int data, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    instance_t			*instance;
    value_t			*value;
    sds				msg;
    int				i;

    /*
     * First satisfy any/all mappings for metric name, instance
     * names, label names and values.  This may issue updates to
     * cache (new) strings.  Then we can issue all (new) metadata
     * and data simultaneously afterward.
     */

    /* ensure all metric name strings are mapped */
    for (i = 0; i < metric->numnames; i++) {
	assert(metric->names[i].sds != NULL);
	seriesBatonReference(baton, "redis_series_metric");
	redisGetMap(slots,
		    namesmap, metric->names[i].id, metric->names[i].sds,
		    series_name_mapping_callback,
		    baton->info, baton->userdata, baton);
    }

    /* ensure all metric or instance label strings are mapped */
    if (metric->desc.indom == PM_INDOM_NULL) {
	series_metric_label_mapping(metric, baton);
    } else {
	for (i = 0; i < metric->u.vlist->listcount; i++) {
	    value = &metric->u.vlist->value[i];
	    if ((instance = dictFetchValue(metric->indom->insts, &value->inst)) == NULL) {
		infofmt(msg, "indom lookup failure for %s instance %u",
				pmInDomStr(metric->indom->indom), value->inst);
		batoninfo(baton, PMLOG_ERROR, msg);
		continue;
	    }
	    assert(instance->name.sds != NULL);
	    seriesBatonReference(baton, "redis_series_metric");
	    redisGetMap(baton->slots,
			instmap, instance->name.id, instance->name.sds,
			series_name_mapping_callback,
			baton->info, baton->userdata, baton);

	    series_instance_label_mapping(metric, instance, baton);
	}
    }

    /* push the metric, instances and any label metadata into the cache */
    if (meta || data)
	redis_series_metadata(&baton->pmapi.context, metric, baton);

    /* push values for all instances, no-value or errors into the cache */
    if (data)
	redis_series_streamed(timestamp, metric, baton);
}

static void
redis_metric_name_series_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;

    checkIntegerReply(load->info, load->userdata,
			reply, "%s %s", SADD, "map metric name to series");
    doneSeriesLoadBaton(arg, "redis_metric_name_series_callback");
}

static void
redis_series_metric_name_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;

    checkIntegerReply(load->info, load->userdata,
			reply, "%s: %s", SADD, "map series to metric name");
    doneSeriesLoadBaton(arg, "redis_series_metric_name_callback");
}

static void
redis_desc_series_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;

    checkStatusReplyOK(load->info, load->userdata,
			reply, "%s: %s", HMSET, "setting metric desc");
    doneSeriesLoadBaton(arg, "redis_desc_series_callback");
}

static void
redis_series_source_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;

    checkIntegerReply(load->info, load->userdata,
			reply, "%s: %s", SADD, "mapping series to context");
    doneSeriesLoadBaton(arg, "redis_series_source_callback");
}

static void
redis_series_metadata(context_t *context, metric_t *metric, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    redisSlots			*slots = baton->slots;
    instance_t			*instance;
    value_t			*value;
    const char			*units, *indom, *pmid, *sem, *type;
    char			ibuf[32], pbuf[32], sbuf[20], tbuf[20], ubuf[60];
    char			hashbuf[42];
    sds				cmd, key;
    int				i;

    indom = pmwebapi_indom_str(metric, ibuf, sizeof(ibuf));
    pmid = pmwebapi_pmid_str(metric, pbuf, sizeof(pbuf));
    sem = pmwebapi_semantics_str(metric, sbuf, sizeof(sbuf));
    type = pmwebapi_type_str(metric, tbuf, sizeof(tbuf));
    units = pmwebapi_units_str(metric, ubuf, sizeof(ubuf));

    for (i = 0; i < metric->numnames; i++) {
	assert(metric->names[i].sds != NULL);

	seriesBatonReferences(baton, 3, "redis_series_metadata names");

	pmwebapi_hash_str(metric->names[i].id, hashbuf, sizeof(hashbuf));
	key = sdscatfmt(sdsempty(), "pcp:series:metric.name:%s", hashbuf);
	cmd = redis_command(3);
	cmd = redis_param_str(cmd, SADD, SADD_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sha(cmd, metric->names[i].hash);
	redisSlotsRequest(slots, SADD, key, cmd,
			redis_series_metric_name_callback, arg);

	pmwebapi_hash_str(metric->names[i].hash, hashbuf, sizeof(hashbuf));
	key = sdscatfmt(sdsempty(), "pcp:metric.name:series:%s", hashbuf);
	cmd = redis_command(3);
	cmd = redis_param_str(cmd, SADD, SADD_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_sha(cmd, metric->names[i].id);
	redisSlotsRequest(slots, SADD, key, cmd,
			redis_metric_name_series_callback, arg);

	key = sdscatfmt(sdsempty(), "pcp:desc:series:%s", hashbuf);
	cmd = redis_command(14);
	cmd = redis_param_str(cmd, HMSET, HMSET_LEN);
	cmd = redis_param_sds(cmd, key);
	cmd = redis_param_str(cmd, "indom", sizeof("indom")-1);
	cmd = redis_param_str(cmd, indom, strlen(indom));
	cmd = redis_param_str(cmd, "pmid", sizeof("pmid")-1);
	cmd = redis_param_str(cmd, pmid, strlen(pmid));
	cmd = redis_param_str(cmd, "semantics", sizeof("semantics")-1);
	cmd = redis_param_str(cmd, sem, strlen(sem));
	cmd = redis_param_str(cmd, "source", sizeof("source")-1);
	cmd = redis_param_sha(cmd, context->name.hash);
	cmd = redis_param_str(cmd, "type", sizeof("type")-1);
	cmd = redis_param_str(cmd, type, strlen(type));
	cmd = redis_param_str(cmd, "units", sizeof("units")-1);
	cmd = redis_param_str(cmd, units, strlen(units));
	redisSlotsRequest(slots, HMSET, key, cmd, redis_desc_series_callback, arg);
    }

    seriesBatonReference(baton, "redis_series_metadata");

    pmwebapi_hash_str(context->name.hash, hashbuf, sizeof(hashbuf));
    key = sdscatfmt(sdsempty(), "pcp:series:source:%s", hashbuf);
    cmd = redis_command(2 + metric->numnames);
    cmd = redis_param_str(cmd, SADD, SADD_LEN);
    cmd = redis_param_sds(cmd, key);
    for (i = 0; i < metric->numnames; i++)
        cmd = redis_param_sha(cmd, metric->names[i].hash);
    redisSlotsRequest(slots, SADD, key, cmd, redis_series_source_callback, arg);

    if (metric->desc.indom == PM_INDOM_NULL) {
	redis_series_labelset(slots, metric, NULL, baton);
    } else {
	for (i = 0; i < metric->u.vlist->listcount; i++) {
	    value = &metric->u.vlist->value[i];
	    if ((instance = dictFetchValue(metric->indom->insts, &value->inst)) == NULL)
		continue;
	    redis_series_instance(slots, metric, instance, baton);
	    redis_series_labelset(slots, metric, instance, baton);
	}
    }
}

typedef struct redisStreamBaton {
    seriesBatonMagic	header;
    redisSlots		*slots;
    sds			stamp;
    char		hash[40+1];
    redisInfoCallBack   info;
    void		*userdata;
    void		*arg;
} redisStreamBaton;

static void
initRedisStreamBaton(redisStreamBaton *baton, redisSlots *slots,
		sds stamp, const char *hash, seriesLoadBaton *load)
{
    initSeriesBatonMagic(baton, MAGIC_STREAM);
    baton->slots = slots;
    baton->stamp = sdsdup(stamp);
    memcpy(baton->hash, hash, sizeof(baton->hash));
    baton->info = load->info;
    baton->userdata = load->userdata;
    baton->arg = load;
}

static void
doneRedisStreamBaton(redisStreamBaton *baton)
{
    void		*load = baton->arg;

    seriesBatonCheckMagic(baton, MAGIC_STREAM, "doneRedisStreamBaton");
    seriesBatonCheckMagic(load, MAGIC_LOAD, "doneRedisStreamBaton");
    memset(baton, 0, sizeof(*baton));
    free(baton);

    doneSeriesLoadBaton(load, "doneRedisStreamBaton");
}

static sds
series_stream_append(sds cmd, sds name, sds value)
{
    unsigned int	nlen = sdslen(name);
    unsigned int	vlen = sdslen(value);

    cmd = sdscatfmt(cmd, "$%u\r\n%S\r\n$%u\r\n%S\r\n", nlen, name, vlen, value);
    sdsfree(value);
    return cmd;
}

static sds
series_stream_value(sds cmd, sds name, int type, pmAtomValue *avp)
{
    unsigned int	bytes;
    const char		*string;
    sds			value;

    if (!avp) {
	value = sdsnewlen("0", 1);
	goto append;
    }

    switch (type) {
    case PM_TYPE_32:
	value = sdscatfmt(sdsempty(), "%i", avp->l);
	break;
    case PM_TYPE_U32:
	value = sdscatfmt(sdsempty(), "%u", avp->ul);
	break;
    case PM_TYPE_64:
	value = sdscatfmt(sdsempty(), "%I", avp->ll);
	break;
    case PM_TYPE_U64:
	value = sdscatfmt(sdsempty(), "%U", avp->ull);
	break;

    case PM_TYPE_FLOAT:
	value = sdscatprintf(sdsempty(), "%e", (double)avp->f);
	break;
    case PM_TYPE_DOUBLE:
	value = sdscatprintf(sdsempty(), "%e", (double)avp->d);
	break;

    case PM_TYPE_STRING:
	if ((string = avp->cp) == NULL)
	    string = "<null>";
	value = sdsnew(string);
	break;

    case PM_TYPE_AGGREGATE:
    case PM_TYPE_AGGREGATE_STATIC:
	if (avp->vbp != NULL) {
	    string = avp->vbp->vbuf;
	    bytes = avp->vbp->vlen - PM_VAL_HDR_SIZE;
	} else {
	    string = "<null>";
	    bytes = strlen(string);
	}
	value = sdsnewlen(string, bytes);
	break;

    default:
	value = sdscatfmt(sdsempty(), "%i", PM_ERR_NYI);
	break;
    }

append:
    return series_stream_append(cmd, name, value);
}

static void
redis_series_stream_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    redisStreamBaton	*baton = (redisStreamBaton *)arg;
    sds			msg;

    seriesBatonCheckMagic(baton, MAGIC_STREAM, "redis_series_stream_callback");
    if (testReplyError(reply, REDIS_ESTREAMXADD)) {
	infofmt(msg, "duplicate or early stream %s insert at time %s",
		baton->hash, baton->stamp);
	batoninfo(baton, PMLOG_WARNING, msg);
    }
    else {
	checkStreamReplyString(baton->info, baton->userdata, reply,
		baton->stamp, "stream %s status mismatch at time %s",
		baton->hash, baton->stamp);
    }
    doneRedisStreamBaton(baton);
}

static void
redis_series_stream(redisSlots *slots, sds stamp, metric_t *metric,
		const char *hash, void *arg)
{
    seriesLoadBaton		*load = (seriesLoadBaton *)arg;
    redisStreamBaton		*baton;
    unsigned int		count;
    int				i, sts, type;
    sds				cmd, key, name, stream = sdsempty();

    if ((baton = malloc(sizeof(redisStreamBaton))) == NULL) {
	stream = sdscatfmt(stream, "OOM creating stream baton");
	batoninfo(load, PMLOG_ERROR, stream);
	return;
    }
    initRedisStreamBaton(baton, slots, stamp, hash, load);
    seriesBatonReference(load, "redis_series_stream");

    count = 3;	/* XADD key stamp */
    key = sdscatfmt(sdsempty(), "pcp:values:series:%s", hash);

    if ((sts = metric->error) < 0) {
	stream = series_stream_append(stream,
			sdsnewlen("-1", 2), sdscatfmt(sdsempty(), "%i", sts));
	count += 2;
    } else {
	name = sdsempty();
	type = metric->desc.type;
	if (metric->desc.indom == PM_INDOM_NULL) {
	    stream = series_stream_value(stream, name, type, &metric->u.atom);
	    count += 2;
	} else if (metric->u.vlist->listcount <= 0) {
	    stream = series_stream_append(stream, sdsnew("0"), sdsnew("0"));
	    count += 2;
	} else {
	    for (i = 0; i < metric->u.vlist->listcount; i++) {
		instance_t	*inst;
		value_t		*v = &metric->u.vlist->value[i];

		if ((inst = dictFetchValue(metric->indom->insts, &v->inst)) == NULL)
		    continue;
		name = sdscpylen(name, (const char *)inst->name.hash, sizeof(inst->name.hash));
		stream = series_stream_value(stream, name, type, &v->atom);
		count += 2;
	    }
	}
	sdsfree(name);
    }

    cmd = redis_command(count);
    cmd = redis_param_str(cmd, XADD, XADD_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_sds(cmd, stamp);
    cmd = redis_param_raw(cmd, stream);
    sdsfree(stream);

    redisSlotsRequest(slots, XADD, key, cmd, redis_series_stream_callback, baton);
}

static void
redis_series_streamed(sds stamp, metric_t *metric, void *arg)
{
    seriesLoadBaton		*baton= (seriesLoadBaton *)arg;
    redisSlots			*slots = baton->slots;
    char			hashbuf[42];
    int				i;

    if (metric->updated == 0)
	return;

    for (i = 0; i < metric->numnames; i++) {
	pmwebapi_hash_str(metric->names[i].hash, hashbuf, sizeof(hashbuf));
	redis_series_stream(slots, stamp, metric, hashbuf, arg);
    }
}

void
redis_series_mark(redisSlots *redis, sds timestamp, int data, void *arg)
{
    seriesLoadBaton		*baton = (seriesLoadBaton *)arg;
    seriesGetContext		*context = &baton->pmapi;

    /* TODO: cache mark records in Redis series, then in done callback... */
    doneSeriesGetContext(context, "redis_series_mark");
}

static void
redis_update_version_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_update_version_callback");
    checkStatusReplyOK(baton->info, baton->userdata, reply,
			"%s setup", "pcp:version:schema");
    doneRedisSlotsBaton(baton);
}

static void
redis_update_version(redisSlotsBaton *baton)
{
    sds			cmd, key;
    const char		ver[] = TO_STRING(SCHEMA_VERSION);

    key = sdsnew("pcp:version:schema");
    cmd = redis_command(3);
    cmd = redis_param_str(cmd, SETS, SETS_LEN);
    cmd = redis_param_sds(cmd, key);
    cmd = redis_param_str(cmd, ver, sizeof(ver)-1);
    redisSlotsRequest(baton->slots, SETS, key, cmd, redis_update_version_callback, baton);
}

static void
redis_load_version_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    unsigned int	version = 0;
    sds			msg;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_load_version_callback");

    if (reply->type == REDIS_REPLY_STRING) {
	version = (unsigned int)atoi(reply->str);
	if (version == 0 || version == SCHEMA_VERSION) {
	    baton->version = version;
	} else {
	    infofmt(msg, "unsupported schema (got v%u, expected v%u)",
			version, SCHEMA_VERSION);
	    batoninfo(baton, PMLOG_ERROR, msg);
	}
    } else if (reply->type == REDIS_REPLY_ERROR) {
	infofmt(msg, "version check error: %s", reply->str);
	batoninfo(baton, PMLOG_REQUEST, msg);
    } else if (reply->type != REDIS_REPLY_NIL) {
	infofmt(msg, "unexpected schema version reply type (%s)",
		redis_reply(reply->type));
	batoninfo(baton, PMLOG_ERROR, msg);
    } else {
	baton->version = 0;	/* NIL - no version key yet */
    }

    /* set the version when none found (first time through) */
    if (version != SCHEMA_VERSION && baton->version != -1)
	redis_update_version(arg);
    else
	doneRedisSlotsBaton(baton);
}

static void
redis_load_version(redisSlotsBaton *baton)
{
    sds			cmd, key;

    key = sdsnew("pcp:version:schema");
    cmd = redis_command(2);
    cmd = redis_param_str(cmd, GETS, GETS_LEN);
    cmd = redis_param_sds(cmd, key);
    redisSlotsRequest(baton->slots, GETS, key, cmd, redis_load_version_callback, baton);
}

static int
decodeCommandKey(redisSlotsBaton *baton, int index, redisReply *reply)
{
    redisSlots		*slots = baton->slots;
    redisReply		*node;
    dictEntry		*entry;
    long long		position;
    sds			msg, cmd;

    /*
     * Each element contains:
     * - command name
     * - command arity specification
     * - nested array reply of command flags
     * - position of first key in argument list
     * - position of last key in argument list
     * - step count for locating repeating keys
     *
     * We care primarily about the command name and position of
     * the first key, as that key is the one used when selecting
     * the Redis server to communicate with for each command, in
     * a setup with more than one server (cluster or otherwise).
     */
    if (reply->elements < 6) {
	infofmt(msg, "bad reply %s[%d] response (%lld elements)",
			COMMAND, index, (long long)reply->elements);
	batoninfo(baton, PMLOG_RESPONSE, msg);
	return -EPROTO;
    }

    node = reply->element[3];
    if ((position = checkIntegerReply(baton->info, baton->userdata, node,
			"KEY position for %s element %d", COMMAND, index)) < 0)
	return -EINVAL;
    node = reply->element[0];
    if ((cmd = checkStringReply(baton->info, baton->userdata, node,
			"NAME for %s element %d", COMMAND, index)) == NULL)
	return -EINVAL;

    if ((entry = dictAddRaw(slots->keymap, cmd, NULL)) != NULL) {
	dictSetSignedIntegerVal(entry, position);
	return 0;
    }
    sdsfree(cmd);
    return -ENOMEM;
}

static void
redis_load_keymap_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    redisReply		*command;
    sds			msg;
    int			i;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_load_keymap_callback");

    if (reply->type == REDIS_REPLY_ARRAY) {
	for (i = 0; i < reply->elements; i++) {
	    command = reply->element[i];
	    if (checkArrayReply(baton->info, baton->userdata,
			command, "%s entry %d", COMMAND, i) == 0)
		decodeCommandKey(baton, i, command);
	}
    } else if (reply->type == REDIS_REPLY_ERROR) {
	infofmt(msg, "command key mapping error: %s", reply->str);
	batoninfo(baton, PMLOG_REQUEST, msg);
    } else if (reply->type != REDIS_REPLY_NIL) {
	infofmt(msg, "unexpected command reply type (%s)",
		redis_reply(reply->type));
	batoninfo(baton, PMLOG_ERROR, msg);
    }

    /* Verify pmseries schema version if previously requested */
    if (baton->flags & SLOTS_VERSION)
	redis_load_version(baton);
    else
	doneRedisSlotsBaton(baton);
}

static void
redis_load_keymap(redisSlotsBaton *baton)
{
    sds			cmd;

    cmd = redis_command(1);
    cmd = redis_param_str(cmd, COMMAND, COMMAND_LEN);
    redisSlotsRequest(baton->slots, GETS, NULL, cmd, redis_load_keymap_callback, baton);
}

static int
decodeRedisNode(redisSlotsBaton *baton, redisReply *reply, redisSlotServer *server)
{
    redisReply		*value;
    unsigned int	port;
    sds			msg;

    /* expecting IP address and port (integer), ignore optional node ID */
    if (reply->elements < 2) {
	infofmt(msg, "insufficient elements in cluster NODE reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }

    value = reply->element[1];
    if (value->type != REDIS_REPLY_INTEGER) {
	infofmt(msg, "expected integer port in cluster NODE reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }
    port = (unsigned int)value->integer;

    value = reply->element[0];
    if (value->type != REDIS_REPLY_STRING) {
	infofmt(msg, "expected string hostspec in cluster NODE reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }

    server->hostspec = sdscatfmt(sdsempty(), "%s:%u", value->str, port);
    return server->hostspec ? 0 : -ENOMEM;
}

static int
decodeRedisSlot(redisSlotsBaton *baton, redisReply *reply)
{
    redisSlotServer	*servers = NULL;
    redisSlotRange	slots, *sp;
    redisReply		*node;
    long long		slot;
    int			i, n;
    sds			msg;

    /* expecting start and end slot range integers, then node arrays */
    if (reply->elements < 3) {
	infofmt(msg, "insufficient elements in cluster SLOT reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }
    memset(&slots, 0, sizeof(slots));

    node = reply->element[0];
    if ((slot = checkIntegerReply(baton->info, baton->userdata,
				node, "%s start", "SLOT")) < 0) {
	infofmt(msg, "expected integer start in cluster SLOT reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }
    slots.start = (__uint32_t)slot;
    node = reply->element[1];
    if ((slot = checkIntegerReply(baton->info, baton->userdata,
				node, "%s end", "SLOT")) < 0) {
	infofmt(msg, "expected integer end in cluster SLOT reply");
	batoninfo(baton, PMLOG_WARNING, msg);
	return -EINVAL;
    }
    slots.end = (__uint32_t)slot;
    node = reply->element[2];
    if ((decodeRedisNode(baton, node, &slots.master)) < 0)
	return -EINVAL;

    if ((sp = calloc(1, sizeof(redisSlotRange))) == NULL)
	return -ENOMEM;
    *sp = slots;    /* struct copy */

    if ((n = reply->elements - 3) > 0)
	if ((servers = calloc(n, sizeof(redisSlotServer))) == NULL)
	    n = 0;
    sp->nreplicas = n;
    sp->replicas = servers;

    for (i = 0; i < n; i++) {
	node = reply->element[i + 3];
	if (checkArrayReply(baton->info, baton->userdata,
				node, "%s range %u-%u replica %d",
				"SLOTS", sp->start, sp->end, i) == 0)
	    decodeRedisNode(baton, node, &sp->replicas[i]);
    }

    return redisSlotRangeInsert(baton->slots, sp);
}

static void
decodeRedisSlots(redisSlotsBaton *baton, redisReply *reply)
{
    redisReply		*slot;
    int			i;

    for (i = 0; i < reply->elements; i++) {
	slot = reply->element[i];
	if (checkArrayReply(baton->info, baton->userdata,
			slot, "%s %s entry %d", CLUSTER, "SLOTS", i) == 0)
	    decodeRedisSlot(baton, slot);
    }
}

static void
redis_load_slots_callback(redisAsyncContext *c, redisReply *reply, void *arg)
{
    redisSlotsBaton	*baton = (redisSlotsBaton *)arg;
    redisSlotServer	*servers;
    redisSlotRange	*slots;
    sds			msg;

    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "redis_load_slots_callback");

    /* Case of a single Redis instance or loosely cooperating instances */
    if (testReplyError(reply, REDIS_ENOCLUSTER)) {
	/* TODO: allow setup of multiple servers via configuration file */
	if ((servers = calloc(1, sizeof(redisSlotServer))) != NULL) {
	    if ((slots = calloc(1, sizeof(redisSlotRange))) != NULL) {
		servers->hostspec = baton->slots->control.hostspec;
		servers->redis = baton->slots->control.redis;
		slots->nreplicas = 0;
		slots->start = 0;
		slots->end = MAXSLOTS;
		slots->master = *servers;
		redisSlotRangeInsert(baton->slots, slots);
	    } else {
		infofmt(msg, "failed to allocate Redis slots memory");
		batoninfo(baton, PMLOG_ERROR, msg);
		free(servers);
		baton->version = -1;
	    }
	}
    }
    /* Case of a cluster of Redis instances, following the cluster spec */
    else {
	if (checkArrayReply(baton->info, baton->userdata,
				reply, "%s %s", CLUSTER, "SLOTS") == 0)
	    decodeRedisSlots(baton, reply);
    }

    if (baton->flags & SLOTS_KEYMAP)
	/* Prepare mapping of commands to key positions if needed */
	redis_load_keymap(baton);
    else if (baton->flags & SLOTS_VERSION)
	/* Verify pmseries schema version if previously requested */
	redis_load_version(baton);
    else
	doneRedisSlotsBaton(baton);
}

static void
redis_load_slots(redisSlotsBaton *baton)
{
    sds			cmd;

    cmd = redis_command(2);
    cmd = redis_param_str(cmd, CLUSTER, CLUSTER_LEN);
    cmd = redis_param_str(cmd, "SLOTS", sizeof("SLOTS")-1);
    redisSlotsRequest(baton->slots, CLUSTER, NULL, cmd, redis_load_slots_callback, baton);
}

void
initRedisSlotsBaton(redisSlotsBaton *baton, redisSlotsFlags flags,
		redisInfoCallBack info, redisDoneCallBack done,
		void *userdata, void *events, void *arg)
{
    initSeriesBatonMagic(baton, MAGIC_SLOTS);
    baton->info = info;
    baton->done = done;
    baton->flags = flags;
    baton->version = -1;
    baton->userdata = userdata;
    baton->arg = arg;
}

void
doneRedisSlotsBaton(redisSlotsBaton *baton)
{
    seriesBatonCheckMagic(baton, MAGIC_SLOTS, "doneRedisSlotsBaton");
    baton->done(baton->arg);
    memset(baton, 0, sizeof(*baton));
    free(baton);
}

redisSlots *
redisSlotsConnect(sds server, redisSlotsFlags flags,
		redisInfoCallBack info, redisDoneCallBack done,
		void *userdata, void *events, void *arg)
{
    redisSlotsBaton		*baton;
    redisSlots			*slots;
    sds				msg;

    if ((baton = (redisSlotsBaton *)calloc(1, sizeof(redisSlotsBaton))) != NULL) {
	if ((slots = redisSlotsInit(server, events)) != NULL) {
	    initRedisSlotsBaton(baton, flags, info, done, userdata, events, arg);
	    baton->slots = slots;
	    redis_load_slots(baton);
	    return slots;
	}
	doneRedisSlotsBaton(baton);
    } else {
	done(arg);
    }
    infofmt(msg, "Failed to allocate memory for Redis slots");
    info(PMLOG_ERROR, msg, arg);
    sdsfree(msg);
    return NULL;
}

void
pmSeriesSetup(pmSeriesModule *module, void *arg)
{
    /* create global EVAL hashes and string map caches */
    redisScriptsInit();
    redisMapsInit();

    /* fast path for when Redis has been setup already */
    if (module->slots) {
	module->on_setup(arg);
    } else {
	/* establish initial basic connection to Redis instances */
	module->slots = redisSlotsConnect(
			module->hostspec, SLOTS_VERSION, module->on_info,
			module->on_setup, arg, module->events, arg);
    }
}

void
pmSeriesClose(pmSeriesModule *module)
{
    redisSlotsFree((redisSlots *)module->slots);
//  memset(module, 0, sizeof(*module));
}

void
pmDiscoverSetup(pmDiscoverSettings *settings, void *arg)
{
    const char		fallback[] = "/var/log/pcp";
    const char		*paths[] = { "pmlogger", "pmmgr" };
    const char		*logdir = pmGetOptionalConfig("PCP_LOG_DIR");
    char		path[MAXPATHLEN];
    char		sep = pmPathSeparator();
    int			i, handle;

    /* create global EVAL hashes and string map caches */
    redisScriptsInit();
    redisMapsInit();

    if (!logdir)
	logdir = fallback;

    for (i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
	pmsprintf(path, sizeof(path), "%s%c%s", logdir, sep, paths[i]);
	if (access(path, F_OK) != 0)
	    continue;
	if ((handle = pmDiscoverRegister(path,
			&settings->module, &settings->callbacks, arg)) < 0)
	    continue;
	/* coverity[DEADCODE] -- this is reached when HAVE_LIBUV is set */
	settings->module.handle = handle;
    }
}

void
pmDiscoverClose(pmDiscoverSettings *settings)
{
    pmDiscoverUnregister(settings->module.handle);
    memset(settings, 0, sizeof(*settings));
}
