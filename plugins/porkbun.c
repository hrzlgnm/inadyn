/* Plugin for PorkBun
*
 * Copyright (C) 2023-2024 Sergio Triana Escobedo
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, visit the Free Software Foundation
 * website at http://www.gnu.org/licenses/gpl-2.0.html or write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "plugin.h"
#include "json.h"

#define CHECK(fn)       { rc = (fn); if (rc) goto cleanup; }

#define API_HOST "api.porkbun.com"
#define API_URL "/client/v4"

/* https://kb.porkbun.com/article/190-getting-started-with-the-porkbun-api */
static const char *PORKBUN_ZONE_ID_REQUEST = "GET " API_URL "/dns/retrieve/%s HTTP/1.0\r\n"	\
	"Host: " API_HOST "\r\n"		\
	"User-Agent: %s\r\n"			\
	"Accept: */*\r\n"				\
	"Content-Type: application/json\r\n" \
	"Content-Length: %zd\r\n\r\n" \
	"{\"apikey\":\"%s\",\"secretapikey\":\"%s\"}";

static const char *PORKBUN_HOSTNAME_ID_REQUEST_BY_NAME	= "GET " API_URL "/dns/retrieve/%s HTTP/1.0\r\n"	\
	"Host: " API_HOST "\r\n"		\
	"User-Agent: %s\r\n"			\
	"Accept: */*\r\n"				\
	"Content-Type: application/json\r\n" \
	"Content-Length: %zd\r\n\r\n" \
	"{\"apikey\":\"%s\",\"secretapikey\":\"%s\"}";

static const char *PORKBUN_HOSTNAME_CREATE_REQUEST	= "POST " API_URL "/dns/create/%s HTTP/1.0\r\n"	\
	"Host: " API_HOST "\r\n"		\
	"User-Agent: %s\r\n"			\
	"Accept: */*\r\n"				\
	"Content-Type: application/json\r\n" \
	"Content-Length: %zd\r\n\r\n" \
	"%s";

static const char *PORKBUN_HOSTNAME_UPDATE_REQUEST	= "POST " API_URL "/dns/edit/%s HTTP/1.0\r\n"	\
	"Host: " API_HOST "\r\n"		\
	"User-Agent: %s\r\n"			\
	"Accept: */*\r\n"				\
	"Content-Type: application/json\r\n" \
	"Content-Length: %zd\r\n\r\n" \
	"%s";

static const char *PORKBUN_UPDATE_JSON_FORMAT = "{\"apikey\":\"%s\",\"secretapikey\":\"%s\",\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"content\":\"%s\",\"ttl\":\"%li\",\"prio\":\"%s\"}";

static const char *IPV4_RECORD_TYPE = "A";
static const char *IPV6_RECORD_TYPE = "AAAA";

static int setup    (ddns_t       *ctx,   ddns_info_t *info, ddns_alias_t *hostname);
static int request  (ddns_t       *ctx,   ddns_info_t *info, ddns_alias_t *hostname);
static int response (http_trans_t *trans, ddns_info_t *info, ddns_alias_t *hostname);

static ddns_system_t plugin = {
	.name         = "default@porkbun.com",

	.setup        = (setup_fn_t)setup,
	.request      = (req_fn_t)request,
	.response     = (rsp_fn_t)response,

	.checkip_name = "checkip.amazonaws.com",
	.checkip_url  = "/",
	.checkip_ssl  = DDNS_CHECKIP_SSL_SUPPORTED,

	.server_name  = API_HOST,
	.server_url   = API_URL
};

/*
 * filled by the setup() callback and handed to ddns_info_t
 * for use later in the request() callback .
 */
#define MAX_ID (32 + 1)

struct pbdata {
	char zone_id[MAX_ID];
	char hostname_id[MAX_ID];
};

static int check_response_code(int status)
{
	switch (status)
	{
	case 200:
		return RC_OK;
	case 400:
		logit(LOG_ERR, "HTTP 400: PorkBun says our request was invalid. Possibly a malformed API key or secret key.");
		return RC_DDNS_RSP_NOTOK;
	case 401:
		logit(LOG_ERR, "HTTP 401: Provided API key or secret key is not valid.");
		return RC_DDNS_RSP_AUTH_FAIL;
	case 429:
		logit(LOG_WARNING, "HTTP 429: We got rate limited.");
		return RC_DDNS_RSP_RETRY_LATER;
	default:
		logit(LOG_WARNING, "HTTP %d: Unexpected status code.", status);
		return RC_DDNS_RSP_NOTOK;
	}
}

static int check_success(const char *json, const jsmntok_t tokens[], const int num_tokens)
{
	int i;

	for (i = 1; i < num_tokens; i++) {
		int set;

		if (jsoneq(json, tokens + i, "success") != 0)
			continue;

		if (i < num_tokens - 1 && json_bool(json, tokens + i + 1, &set) == 0)
			return set ? 0 : -1;

		return -1;
	}

	return -1;
}

static int check_success_only(const char *json)
{
	jsmntok_t *tokens;
	int num_tokens;
	int result;

	num_tokens = parse_json(json, &tokens);
	if (num_tokens == -1)
		return -1;

	result = check_success(json, tokens, num_tokens);
	free(tokens);

	return result;
}

static int get_result_value(const char *json, const char *key, jsmntok_t *out_result)
{
	jsmntok_t *tokens;
	int num_tokens;
	int i = 0;

	num_tokens = parse_json(json, &tokens);
	if (num_tokens < 0)
		return -1;

	if (tokens[0].type != JSMN_OBJECT) {
		logit(LOG_ERR, "JSON response contained no objects.");
		goto cleanup;
	}

	if (check_success(json, tokens + i + 1, num_tokens) == -1) {
		logit(LOG_ERR, "Request was unsuccessful.");
		goto cleanup;
	}

	for (i = 1; i < num_tokens; i++) {
		if (jsoneq(json, tokens + i, key) != 0)
			continue;

		if (i < num_tokens - 1) {
			*out_result = tokens[i+1];
			free(tokens);
			return 0;
		}
	}

	logit(LOG_INFO, "Could not find key '%s'.", key);

	cleanup:
		free(tokens);
	return -1;
}

static int json_copy_value(char *dest, size_t dest_size, const char *json, const jsmntok_t *token)
{
	size_t length;

	if (token->type != JSMN_STRING)
		return -1;

	length = token->end - token->start + 1;
	if (length > dest_size)
		return -2;

	strlcpy(dest, json + token->start, length);

	return 0;
}

static int json_extract(char *dest, size_t dest_size, const ddns_info_t *info, char *request, size_t request_len, const char *key)
{
	const char   *body;
	http_trans_t  trans;
	jsmntok_t     key_value;
	http_t        client;
	char         *response_buf;
	size_t        response_buflen = DDNS_HTTP_RESPONSE_BUFFER_SIZE;
	int           rc = RC_OK;

	response_buf = calloc(response_buflen, sizeof(char));
	if (!response_buf)
		return RC_OUT_OF_MEMORY;

	CHECK(http_construct(&client));

	http_set_port(&client, info->server_name.port);
	http_set_remote_name(&client, info->server_name.name);

	client.ssl_enabled = info->ssl_enabled;
	CHECK(http_init(&client, "Json query", ddns_get_tcp_force(info)));

	trans.req = request;
	trans.req_len = request_len;
	trans.rsp = response_buf;
	trans.max_rsp_len = response_buflen - 1; /* Save place for a \0 at the end */

	logit(LOG_DEBUG, "Request:\n%s", request);
	CHECK(http_transaction(&client, &trans));

	http_exit(&client);
	http_destruct(&client, 1);

	logit(LOG_DEBUG, "Response:\n%s", trans.rsp);
	CHECK(check_response_code(trans.status));

	body = trans.rsp_body;
	if (get_result_value(body, key, &key_value) < 0) {
		rc = RC_DDNS_RSP_NOHOST;
		goto cleanup;
	}

	if (json_copy_value(dest, dest_size, body, &key_value) < 0) {
		logit(LOG_ERR, "Key value did not fit into buffer.");
		rc = RC_BUFFER_OVERFLOW;
	}
	logit(LOG_DEBUG, "Key '%s' = %s", key, dest);

	cleanup:
		free(response_buf);

	return rc;
}

static const char* get_record_type(const char *address)
{
	if (strstr(address, ":"))
		return IPV6_RECORD_TYPE;

	return IPV4_RECORD_TYPE;
}

static int setup(ddns_t *ctx, ddns_info_t *info, ddns_alias_t *hostname)
{
	const char *zone_name = hostname->name;
	struct pbdata *data;
	int rc = RC_OK;
	size_t len;

	if (*zone_name == '\0' || !strchr(zone_name, '.'))
	{
		logit(LOG_ERR, "Invalid zone. Enter the PorkBun domain in the hostname field.");
		return RC_DDNS_INVALID_OPTION;
	}

	data = calloc(1, sizeof(struct pbdata));
	if (!data)
		return RC_OUT_OF_MEMORY;

	if (info->data)
		free(info->data);
	info->data = data;

	logit(LOG_DEBUG, "Zone: %s", zone_name);

	len = snprintf(ctx->request_buf, ctx->request_buflen,
		       PORKBUN_ZONE_ID_REQUEST,
		       zone_name,
		       info->user_agent,
		       strlen(info->creds.username),
		       info->creds.username,
		       info->creds.password);

	if (len >= ctx->request_buflen) {
		logit(LOG_ERR, "Request for zone '%s' did not fit into buffer.", zone_name);
		return RC_BUFFER_OVERFLOW;
	}

	rc = json_extract(data->zone_id, MAX_ID, info, ctx->request_buf, len, "id");
	if (rc != RC_OK) {
		logit(LOG_ERR, "Zone '%s' not found.", zone_name);
		return rc;
	}

	logit(LOG_DEBUG, "PorkBun Zone: '%s' Id: %s", zone_name, data->zone_id);

	/* Query the unique porkbun id from hostname.
	   If more than one record is returned (round-robin dns) use only the first and ignore the others. */
	len = snprintf(ctx->request_buf, ctx->request_buflen,
			PORKBUN_HOSTNAME_ID_REQUEST_BY_NAME,
			data->zone_id,
			info->user_agent,
			strlen(info->creds.username),
			info->creds.username,
			info->creds.password);
	if (len >= ctx->request_buflen) {
		logit(LOG_ERR, "Request for zone '%s', id %s did not fit into buffer.",
			zone_name, data->zone_id);
		return RC_BUFFER_OVERFLOW;
	}

	rc = json_extract(data->hostname_id, MAX_ID, info, ctx->request_buf, ctx->request_buflen, "id");

	if (rc == RC_OK) {
		logit(LOG_DEBUG, "PorkBun Host: '%s' Id: %s", hostname->name, data->hostname_id);
	} else if (rc == RC_DDNS_RSP_NOHOST) {
		strcpy(data->hostname_id, "");
		return RC_OK;
	} else {
		logit(LOG_INFO, "Hostname '%s' not found.", hostname->name);
	}

	return rc;
}

static int request(ddns_t *ctx, ddns_info_t *info, ddns_alias_t *hostname)
{
	const char *record_type;
	struct pbdata *data = (struct pbdata *)info->data;
	size_t content_len;
	char json_data[256];

	record_type = get_record_type(hostname->address);
	content_len = snprintf(json_data, sizeof(json_data),
				   PORKBUN_UPDATE_JSON_FORMAT,
				   info->creds.username, // API key
				   info->creds.password, // Secret key
				   data->hostname_id,
				   hostname->name,
				   record_type,
				   hostname->address,
				   info->ttl >= 0 ? info-> ttl : 300, // Time to live for DNS record. Default is 300
				   "0"); // Priority. Only used for MX and SRV records

	if (strlen(data->hostname_id) == 0)
		return snprintf(ctx->request_buf, ctx->request_buflen,
			PORKBUN_HOSTNAME_CREATE_REQUEST,
			data->zone_id,
			info->user_agent,
			content_len, json_data);

	return snprintf(ctx->request_buf, ctx->request_buflen,
			PORKBUN_HOSTNAME_UPDATE_REQUEST,
			data->zone_id,
			info->user_agent,
			content_len, json_data);
}

static int response(http_trans_t *trans, ddns_info_t *info, ddns_alias_t *hostname)
{
	int rc;

	(void)info;
	(void)hostname;

	rc = check_response_code(trans->status);
	if (rc == RC_OK && check_success_only(trans->rsp_body) < 0)
		rc = RC_DDNS_RSP_NOTOK;

	return rc;
}

PLUGIN_INIT(plugin_init)
{
	plugin_register(&plugin, PORKBUN_HOSTNAME_UPDATE_REQUEST);
	plugin_register_v6(&plugin, PORKBUN_HOSTNAME_UPDATE_REQUEST);
}

PLUGIN_EXIT(plugin_exit)
{
	plugin_unregister(&plugin);
}
