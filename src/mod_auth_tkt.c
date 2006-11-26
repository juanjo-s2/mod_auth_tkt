
#include <netinet/in.h>
#include <arpa/inet.h>

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_core.h"
#include "http_protocol.h"
#include "util_md5.h"

#ifdef APACHE13
#include "ap_compat.h"
#else
#include "apr_lib.h"
#include "apr_strings.h"
#include "apr_uuid.h"
#include "apr_base64.h"
#include "pcreposix.h"
#define UUID_SUBS 2
#endif

#define AUTH_COOKIE_NAME "auth_tkt"
#define BACK_ARG_NAME "back"
#define MD5_DIGEST_SZ 32
#define MD5andTSTAMP (MD5_DIGEST_SZ + 8)
#define SEPARATOR '!'
#define SEPARATOR_HEX "%21"
#define REMOTE_USER_ENV "REMOTE_USER"
#define REMOTE_USER_DATA_ENV "REMOTE_USER_DATA"
#define REMOTE_USER_TOKENS_ENV "REMOTE_USER_TOKENS"
#define DEFAULT_TIMEOUT_SEC 7200
#define DEFAULT_GUEST_USER "guest"

#define FORCE_REFRESH 1
#define CHECK_REFRESH 0

#define TKT_AUTH_VERSION "2.0.0rc1"

/* ----------------------------------------------------------------------- */
/* Per-directory configuration */
typedef struct  {
  char *directory;
  char *login_url;
  char *timeout_url;
  char *post_timeout_url;
  char *unauth_url;
  char *auth_domain;
  int cookie_expires;
  char *auth_cookie_name;
  char *back_cookie_name;
  char *back_arg_name;
  apr_array_header_t *auth_token;
  int ignore_ip;
  int require_ssl;
  int secure_cookie;
  int timeout_sec;
  double timeout_refresh;
  int guest_login;
  int guest_cookie;
  char *guest_user;
  int debug;
} auth_tkt_dir_conf;

/* Per-server configuration */
typedef struct {
  const char *secret;
  char *docroot;
} auth_tkt_serv_conf;

typedef struct auth_tkt_struct {
  char *uid;
  char *tokens;
  char *user_data;
  unsigned int timestamp;
} auth_tkt;

typedef struct {
  request_rec *r;
  char *cookie;
  char *cookie_name;
} cookie_res;

/* ----------------------------------------------------------------------- */
/* Initializer */
#ifdef APACHE13
void
auth_tkt_version(server_rec *s, pool *p)
{
  ap_add_version_component("mod_auth_tkt/" TKT_AUTH_VERSION);
  ap_log_error(APLOG_MARK, APLOG_INFO, APR_SUCCESS, s, 
      "mod_auth_tkt: version %s", TKT_AUTH_VERSION);
}

#else
static int
auth_tkt_version(apr_pool_t *p, 
  apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
  ap_add_version_component(p, "mod_auth_tkt/" TKT_AUTH_VERSION);
  ap_log_error(APLOG_MARK, APLOG_INFO, APR_SUCCESS, s, 
      "mod_auth_tkt: version %s", TKT_AUTH_VERSION);
  return DECLINED;
}
#endif

/* Create per-dir config structures */
static void * 
create_auth_tkt_config(apr_pool_t *p, char* path)
{
  auth_tkt_dir_conf *conf = apr_palloc(p, sizeof(*conf));

  conf->directory = path;
  conf->login_url = NULL;
  conf->timeout_url = NULL;
  conf->post_timeout_url = NULL;
  conf->unauth_url = NULL;
  conf->auth_domain = NULL;
  conf->cookie_expires = -1;
  conf->auth_token = apr_array_make(p, 0, sizeof (char *));
  conf->auth_cookie_name = AUTH_COOKIE_NAME;
  conf->back_cookie_name = NULL;
  conf->back_arg_name = BACK_ARG_NAME;
  conf->ignore_ip = -1;
  conf->require_ssl = -1;
  conf->secure_cookie = -1;
  conf->timeout_sec = -1;
  conf->timeout_refresh = .5;
  conf->guest_login = -1;
  conf->guest_cookie = -1;
  conf->guest_user = NULL;
  conf->debug = -1;
  return conf;  
}

/* Merge per-dir config structures */
static void * 
merge_auth_tkt_config(apr_pool_t *p, void* parent_dirv, void* subdirv)
{
  auth_tkt_dir_conf *parent = (auth_tkt_dir_conf *) parent_dirv;
  auth_tkt_dir_conf *subdir = (auth_tkt_dir_conf *) subdirv;
  auth_tkt_dir_conf *conf = apr_palloc(p, sizeof(*conf));

  conf->directory = (subdir->directory) ? subdir->directory : parent->directory;
  conf->login_url = (subdir->login_url) ? subdir->login_url : parent->login_url;
  conf->timeout_url = (subdir->timeout_url) ? subdir->timeout_url : parent->timeout_url;
  conf->post_timeout_url = (subdir->post_timeout_url) ? subdir->post_timeout_url : parent->post_timeout_url;
  conf->unauth_url = (subdir->unauth_url) ? subdir->unauth_url : parent->unauth_url;
  conf->auth_domain = (subdir->auth_domain) ? subdir->auth_domain : parent->auth_domain;
  conf->cookie_expires = (subdir->cookie_expires >= 0) ? subdir->cookie_expires : parent->cookie_expires;
  conf->auth_token = (subdir->auth_token->nelts > 0) ? subdir->auth_token : parent->auth_token;
  conf->auth_cookie_name = (subdir->auth_cookie_name) ? subdir->auth_cookie_name : parent->auth_cookie_name;
  conf->back_cookie_name = (subdir->back_cookie_name) ? subdir->back_cookie_name : parent->back_cookie_name;
  conf->back_arg_name = (subdir->back_arg_name) ? subdir->back_arg_name : parent->back_arg_name;
  conf->ignore_ip = (subdir->ignore_ip >= 0) ? subdir->ignore_ip : parent->ignore_ip;
  conf->require_ssl = (subdir->require_ssl >= 0) ? subdir->require_ssl : parent->require_ssl;
  conf->secure_cookie = (subdir->secure_cookie >= 0) ? subdir->secure_cookie : parent->secure_cookie;
  conf->timeout_sec = (subdir->timeout_sec >= 0) ? subdir->timeout_sec : parent->timeout_sec;
  conf->timeout_refresh = (subdir->timeout_refresh >= 0) ? subdir->timeout_refresh : parent->timeout_refresh;
  conf->guest_login = (subdir->guest_login >= 0) ? subdir->guest_login : parent->guest_login;
  conf->guest_cookie = (subdir->guest_cookie >= 0) ? subdir->guest_cookie : parent->guest_cookie;
  conf->guest_user = (subdir->guest_user) ? subdir->guest_user : parent->guest_user;
  conf->debug = (subdir->debug >= 0) ? subdir->debug : parent->debug;

  return conf;
}

/* Create per-server config structures */
static void *
create_auth_tkt_serv_config(apr_pool_t *p, server_rec* s)
{
  auth_tkt_serv_conf *conf = apr_palloc(p, sizeof(*conf));
  conf->secret = NULL;
  return conf;
} 

/* Merge per-server config structures */
static void *
merge_auth_tkt_serv_config(apr_pool_t *p, void* parent_dirv, void* subdirv)
{
  auth_tkt_serv_conf *parent = (auth_tkt_serv_conf *) parent_dirv;
  auth_tkt_serv_conf *subdir = (auth_tkt_serv_conf *) subdirv;
  auth_tkt_serv_conf *conf = apr_palloc(p, sizeof(*conf));

  conf->secret = (subdir->secret) ? subdir->secret : parent->secret;
  return conf;
} 

/* ----------------------------------------------------------------------- */
/* Command-specific functions */

module AP_MODULE_DECLARE_DATA auth_tkt_module;

/* Loosely based on mod_expires */
static const char *
convert_to_seconds (cmd_parms *cmd, const char *param, int *seconds)
{
  int num, multiplier;
  char unit;

  if (apr_isdigit(param[0])) {
    num = atoi(param);
  }
  else {
    return "Bad time string - numeric expected.";
  }

  if (*seconds < 0) *seconds = 0;
  multiplier = 1;

  unit = param[strlen(param)-1];
  if (apr_isalpha(unit)) {
    if (unit == 's')
      multiplier = 1;
    else if (unit == 'm')
      multiplier = 60;
    else if (unit == 'h')
      multiplier = 60 * 60;
    else if (unit == 'd')
      multiplier = 24 * 60 * 60;
    else if (unit == 'w')
      multiplier = 7 * 24 * 60 * 60;
    else if (unit == 'M')
      multiplier = 30 * 24 * 60 * 60;
    else if (unit == 'y')
      multiplier = 365 * 24 * 60 * 60;
    else 
      return apr_psprintf(cmd->pool, 
        "Bad time string - unrecognised unit '%c'", unit);
  }

  *seconds += num * multiplier;

  return NULL;
}

static const char *
set_auth_tkt_token (cmd_parms *cmd, void *cfg, const char *param)
{
  char	**new;
  auth_tkt_dir_conf *conf = (auth_tkt_dir_conf *) cfg;

  new = (char **) apr_array_push(conf->auth_token);
  *new = apr_pstrdup(cmd->pool, param);
  return NULL;
}

static const char *
set_auth_tkt_timeout (cmd_parms *cmd, void *cfg, const char *param)
{
  auth_tkt_dir_conf *conf = (auth_tkt_dir_conf *)cfg;
  int seconds = conf->timeout_sec;
  const char *error;
        
  /* Easy case - looks like all digits */
  if (apr_isdigit(param[0]) && apr_isdigit(param[strlen(param) - 1])) {
    seconds = atoi(param);
  }
         
  /* Harder case - convert units to seconds */
  else {
    error = convert_to_seconds(cmd, param, &seconds);
    if (error) return(error);
  }

  if (seconds < 0)        return ("Timeout must be positive");
  if (seconds == INT_MAX) return ("Integer overflow or invalid number");

  conf->timeout_sec = seconds;
  
  return NULL;
}

static const char *
set_auth_tkt_timeout_min (cmd_parms *cmd, void *cfg, const char *param)
{
  auth_tkt_dir_conf *conf = (auth_tkt_dir_conf *)cfg;
  
  int minutes = atoi(param);
  
  if (minutes < 0)        return ("Timeout must be positive");
  if (minutes == INT_MAX) return ("Integer overflow or invalid number");
  
  conf->timeout_sec = minutes * 60;
  
  return NULL;
}

static const char *
set_auth_tkt_timeout_refresh (cmd_parms *cmd, void *cfg, const char *param)
{
  auth_tkt_dir_conf *conf = (auth_tkt_dir_conf *)cfg;
  
  double refresh = atof(param);

  if (refresh < 0 || refresh > 1) 
    return "Refresh flag must be between 0 and 1";
  
  conf->timeout_refresh = refresh;
  
  return NULL;
}

static const char *
setup_secret (cmd_parms *cmd, void *cfg, const char *param)
{
  auth_tkt_serv_conf *sconf = ap_get_module_config(cmd->server->module_config, 
    &auth_tkt_module);
  sconf->secret = param;
  return NULL;
}

static const char *
set_cookie_expires (cmd_parms *cmd, void *cfg, const char *param)
{
  auth_tkt_dir_conf *conf = (auth_tkt_dir_conf *)cfg;
  int seconds = conf->cookie_expires;
  const char *error;

  /* Easy case - looks like all digits */
  if (apr_isdigit(param[0]) && apr_isdigit(param[strlen(param) - 1])) {
    seconds = atoi(param);
  }

  /* Harder case - convert units to seconds */
  else {
    error = convert_to_seconds(cmd, param, &seconds);
    if (error) return(error);
  }

  if (seconds < 0)        return ("Expires must be positive");
  if (seconds == INT_MAX) return ("Integer overflow or invalid number");

  conf->cookie_expires = seconds;
  
  return NULL;
}

static const char *
set_auth_tkt_debug (cmd_parms *cmd, void *cfg, const char *param)
{
  auth_tkt_dir_conf *conf = (auth_tkt_dir_conf *)cfg;
  
  int debug = atoi(param);
  
  if (debug < 0)        return ("Debug level must be positive");
  if (debug == INT_MAX) return ("Integer overflow or invalid number");
  
  conf->debug = debug;
  
  return NULL;
}

/* Command table */
static const command_rec auth_tkt_cmds[] =
{
  AP_INIT_TAKE1("TKTAuthLoginURL", ap_set_string_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, login_url),
    OR_AUTHCFG, "URL to redirect to if authentication fails"),
  AP_INIT_TAKE1("TKTAuthTimeoutURL", ap_set_string_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, timeout_url),
    OR_AUTHCFG, "URL to redirect to if cookie times-out"),
  AP_INIT_TAKE1("TKTAuthPostTimeoutURL", ap_set_string_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, post_timeout_url),
    OR_AUTHCFG, "URL to redirect to if cookie times-out doing a POST"),
  AP_INIT_TAKE1("TKTAuthUnauthURL", ap_set_string_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, unauth_url),
    OR_AUTHCFG, "URL to redirect to if valid user without required token"),
  AP_INIT_TAKE1("TKTAuthCookieName", ap_set_string_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, auth_cookie_name),
    OR_AUTHCFG, "name to use for ticket cookie"),
  AP_INIT_TAKE1("TKTAuthDomain", ap_set_string_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, auth_domain),
    OR_AUTHCFG, "domain to use in cookies"),
#ifndef APACHE13
  /* TKTAuthCookieExpires not supported under Apache 1.3 yet */
  AP_INIT_ITERATE("TKTAuthCookieExpires", set_cookie_expires, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, cookie_expires),
    OR_AUTHCFG, "cookie expiry period, in seconds or units [smhdwMy]"),
#endif
  AP_INIT_TAKE1("TKTAuthBackCookieName", ap_set_string_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, back_cookie_name),
    OR_AUTHCFG, "name to use for back cookie (NULL for none)"),
  AP_INIT_TAKE1("TKTAuthBackArgName", ap_set_string_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, back_arg_name),
    OR_AUTHCFG, "name to use for back url argument (NULL for none)"),
  AP_INIT_FLAG("TKTAuthIgnoreIP", ap_set_flag_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, ignore_ip),
    OR_AUTHCFG, "whether to ignore remote IP address in ticket"),
  AP_INIT_FLAG("TKTAuthRequireSSL", ap_set_flag_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, require_ssl),
    OR_AUTHCFG, "whether to refuse non-HTTPS requests"),
  AP_INIT_FLAG("TKTAuthCookieSecure", ap_set_flag_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, secure_cookie),
    OR_AUTHCFG, "whether to set secure flag on ticket cookies"),
  AP_INIT_ITERATE("TKTAuthToken", set_auth_tkt_token, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, auth_token),
    OR_AUTHCFG, "token required to access this area (NULL for none)"),
  AP_INIT_ITERATE("TKTAuthTimeout", set_auth_tkt_timeout, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, timeout_sec),
    OR_AUTHCFG, "ticket inactivity timeout, in seconds or units [smhdwMy]"),
  AP_INIT_TAKE1("TKTAuthTimeoutMin", set_auth_tkt_timeout_min, 
    NULL, OR_AUTHCFG, "ticket inactivity timeout, in minutes (deprecated)"),
  AP_INIT_TAKE1("TKTAuthTimeoutRefresh", set_auth_tkt_timeout_refresh, 
    NULL, OR_AUTHCFG, "ticket timeout refresh flag (0-1)"),
  AP_INIT_TAKE1("TKTAuthSecret", setup_secret, 
    NULL, RSRC_CONF, "secret key to use in MD5 digest"),
  AP_INIT_FLAG("TKTAuthGuestLogin", ap_set_flag_slot,
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, guest_login),
    OR_AUTHCFG, "whether to log people in as guest if no other auth available"),
  AP_INIT_FLAG("TKTAuthGuestCookie", ap_set_flag_slot,
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, guest_cookie),
    OR_AUTHCFG, "whether to set a cookie when accepting guest users (default off)"),
  AP_INIT_TAKE1("TKTAuthGuestUser", ap_set_string_slot, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, guest_user),
    OR_AUTHCFG, "username to use for guest logins"),
  AP_INIT_ITERATE("TKTAuthDebug", set_auth_tkt_debug, 
    (void *)APR_OFFSETOF(auth_tkt_dir_conf, debug),
    OR_AUTHCFG, "debug level (1-3, higher for more debug output)"),
  {NULL},
};

/* ----------------------------------------------------------------------- */
/* Support functions */

/* Parse cookie. Returns 1 if valid, and details in *parsed; 0 if not */
static int 
parse_ticket(request_rec *r, char **magic, auth_tkt *parsed)
{
  int sepidx, sep2idx;
  char *ticket = *magic;
  int len = strlen(ticket);
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);
  
  /* For some reason (some clients?), tickets sometimes come in quoted */
  if (ticket[len-1] == '"') ticket[len-1] = 0;
  if (ticket[0] == '"') *magic = ++ticket;

  /* Basic length check for min size */
  if (len <= MD5andTSTAMP) return 0; 
  
  /* See if there is a uid/data separator */
  sepidx = ap_ind(ticket, SEPARATOR);
  if (sepidx == -1) {	
    /* Ticket either uri-escaped, base64-escaped, or bogus */
    if (strstr(ticket, SEPARATOR_HEX)) {
      ap_unescape_url(ticket);
      sepidx = ap_ind(ticket, SEPARATOR);
    }
    else {
      /* base64 encoded string always longer than original, so len+1 sufficient */
      char *buf = (char *) apr_palloc(r->pool, len+1);  
      apr_base64_decode(buf, ticket);
      sepidx = ap_ind(buf, SEPARATOR);
      /* If still no sepidx, must be bogus */
      if (sepidx == -1) return 0;
      /* Update ticket and *magic to decoded version */
      ticket = *magic = buf;
    }
    /* Reset len */
    len = strlen(ticket);
  }

  /* Recheck length */
  if (len <= MD5andTSTAMP || sepidx < MD5andTSTAMP) return 0; 

  if (conf->debug >= 1) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT parse_ticket decoded ticket: '%s'", ticket);
  }
  
  /* Get the user id */
  parsed->uid = apr_palloc(r->pool, sepidx - MD5andTSTAMP + 1);
  memcpy(parsed->uid, &ticket[MD5andTSTAMP], sepidx - MD5andTSTAMP);
  parsed->uid[sepidx - MD5andTSTAMP] = '\0';
  
  /* Check for tokens */
  sep2idx = ap_ind(&ticket[sepidx+1], SEPARATOR);
  if (sep2idx == -1) {
    if (conf->debug >= 2) {
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
         "TKT parse_ticket: no tokens");
    }
    parsed->tokens = apr_palloc(r->pool, 1);
    *parsed->tokens = '\0';
  }
  else {
    /* Swap sepidx and sep2idx */
    int tmp = sepidx;
    sepidx = tmp + sep2idx + 1;
    sep2idx = tmp;
    if (conf->debug >= 2) {
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
        "TKT parse_ticket: tokens found - sep2=%d, sep=%d, len=%d", 
	sep2idx, sepidx, len);
    }
    /* Copy tokens to parsed->tokens */
    parsed->tokens = apr_palloc(r->pool, sepidx-sep2idx);
    apr_snprintf(parsed->tokens, sepidx-sep2idx, "%s", &ticket[sep2idx+1]);
    if (conf->debug >= 2) {
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
        "TKT parse_ticket tokens: '%s'", parsed->tokens);
    }
  }

  /* Copy user data to parsed->user_data */
  parsed->user_data = apr_palloc(r->pool, len-sepidx+1);
  apr_snprintf(parsed->user_data, len-sepidx+1, "%s", &ticket[sepidx+1]);
  
  /* Copy timestamp to parsed->timestamp */
  sscanf(&ticket[MD5_DIGEST_SZ], "%8x", &(parsed->timestamp));
  
  return 1;
}

/* Search cookie headers for our ticket */
static int 
cookie_match(void *result, const char *key, const char *cookie)
{
  cookie_res * cr = (cookie_res *) result;
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(cr->r->per_dir_config, &auth_tkt_module);
  
  if (cookie != NULL) {
    char *cookie_name, *value, *cookiebuf, *end;
    if (conf->debug >= 2) {
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, cr->r, 
        "TKT cookie_match, key %s against <%s> (name=%s)",
        key, cookie, cr->cookie_name);
    }

    cookie_name = apr_palloc(cr->r->pool, strlen(cr->cookie_name) + 2);
    strncpy(cookie_name, cr->cookie_name, strlen(cr->cookie_name));
    cookie_name[strlen(cr->cookie_name)] = '=';
    cookie_name[strlen(cr->cookie_name) + 1] = '\0';

    value = (char*) cookie;
    while ((value = strstr(value, cookie_name))) {
      /* Cookie includes our cookie_name - copy (first) value into cookiebuf */
      value += strlen(cookie_name);
      cookiebuf = apr_pstrdup(cr->r->pool, value);
      end = ap_strchr(cookiebuf, ';');
      if (end) *end = '\0';      /* Ignore anything after the next ; */
      /* Skip empty cookies (such as with misconfigured logoffs) */
      if (strlen(cookiebuf)) {
        cr->cookie = cookiebuf;
        if (conf->debug >= 1) {
          ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, cr->r, 
            "TKT cookie_match: found '%s'", cookiebuf);
        }
        return(0);
      }
    }
  }
  if (conf->debug >= 2) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, cr->r, 
     "TKT cookie_match: NOT found");
  }
  return (1);
}

/* Return the domain to use in cookies */
char *
get_domain(request_rec *r, auth_tkt_dir_conf *conf) 
{
  /* Default to server name if not explicitly set. Viljo Viitanen
     pointed out that using the wildcard domain is a security hole
     in the event that other servers on your domain are hostile. */
  char *domain = conf->auth_domain;
  if (!domain) domain = (char *) r->hostname;
  return domain;
}

/* Send an auth cookie with the given value */
static void
send_auth_cookie(request_rec *r, char *value)
{
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);
  char *cookie, *expires;
  char *domain = get_domain(r,conf);
  char *secure_cookie = conf->secure_cookie > 0 ? "; secure" : "";

  /* Set cookie domain */
  domain = domain ?  apr_psprintf(r->pool, "; domain=%s", domain) : "";

  /* Set cookie expires */
  expires = "";
#ifndef APACHE13
  if (conf->cookie_expires > 0) {
    apr_time_exp_t tms;
    apr_time_exp_gmt(&tms, r->request_time + 
      apr_time_from_sec(conf->cookie_expires));
    expires = 
      apr_psprintf(r->pool, "; expires=%s, %.2d-%s-%.2d %.2d:%.2d:%.2d GMT",
        apr_day_snames[tms.tm_wday],
        tms.tm_mday, 
        apr_month_snames[tms.tm_mon],
        tms.tm_year % 100,
        tms.tm_hour, tms.tm_min, tms.tm_sec
      );
  }
#endif

  /* Send the cookie */
  cookie = apr_psprintf(r->pool, "%s=%s; path=/%s%s%s", 
    conf->auth_cookie_name, value, domain, expires, secure_cookie);
  apr_table_setn(r->err_headers_out, "Set-Cookie", cookie);

  if (conf->debug >= 1) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT: sending cookie: %s=%s; path=/%s%s%s",
        conf->auth_cookie_name, value, domain, expires, secure_cookie);
  }
}

/* Look for a url ticket */
static char *
get_url_ticket(request_rec *r)
{
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);
  const char *args = NULL;  /* url arguments string  */
  const char *key, *val;
  char *ticket = NULL;

  /* Use main request args if subrequest */
  request_rec *r_main = r->main == NULL ? r : r->main;
  if (r_main->args != NULL) {
    args = apr_pstrdup(r->pool, r_main->args); 
  }

  if (args != NULL) {
    if (conf->debug >= 1) {
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
        "TKT: looking for ticket in url: <%s>", args);
    }

    while (*args && (val = ap_getword(r->pool, &args, '&'))) {
      key = ap_getword(r->pool, &val, '=');

      if (strcmp(key,conf->auth_cookie_name) == 0) {
        if (conf->debug >= 1) {
          ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
            "TKT: found url ticket: <%s>", val);
        }

        /* Setup auth cookie using ticket value */
        send_auth_cookie(r, (char *) val); 

        /* Found ticket - ignore rest of arguments */
        ticket = (char *) val;
        break;
      }
    }
  }

  return ticket;
}

/* Look for a cookie ticket */
static char * 
get_cookie_ticket(request_rec *r)
{
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);

  /* Walk cookie headers looking for matching ticket */
  cookie_res *cr = apr_palloc(r->pool, sizeof(*cr));
  cr->r = r;
  cr->cookie = NULL;
  cr->cookie_name = conf->auth_cookie_name;
  apr_table_do(cookie_match, (void *) cr, r->headers_in, "Cookie", NULL);

  /* Give up if cookie not found or too short */
  if (! cr->cookie || strlen(cr->cookie) < MD5andTSTAMP) return NULL;

  return cr->cookie;
}

/* Generate a ticket digest string from the given details */
static char * 
ticket_digest(request_rec *r, auth_tkt *parsed, unsigned int timestamp)
{
  auth_tkt_serv_conf *sconf =
    ap_get_module_config(r->server->module_config, &auth_tkt_module);
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);
  const char *secret = sconf->secret;
  char *uid = parsed->uid;
  char *tokens = parsed->tokens;
  char *user_data = parsed->user_data;

  unsigned char *buf = apr_palloc(r->pool, 8 + strlen(secret) + strlen(uid) + 1 + strlen(tokens) + 1 + strlen(user_data) + 1);
  unsigned char *buf2 = apr_palloc(r->pool, MD5_DIGEST_SZ + strlen(secret));
  int len = 0;
  char *digest;
  char *remote_ip = conf->ignore_ip > 0 ? "0.0.0.0" : r->connection->remote_ip;
  unsigned long ip;
  struct in_addr ia;

  /* Convert remote_ip to unsigned long */
  if (inet_aton(remote_ip, &ia) == 0) {
    return (NULL);
  }
  ip = ntohl(ia.s_addr);

  if (timestamp == 0) timestamp = parsed->timestamp;

  if (conf->debug >= 2) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT ticket_digest: using md5 key '%s', ip '%s', ts '%d'", secret, remote_ip, timestamp);
  }

  /* Fatals */
  if (buf == NULL) return (NULL);
  if (ip == APR_INADDR_NONE) return (NULL);

  /* First 8 bytes for ip address + timestamp */
  buf[0] = (unsigned char ) ((ip & 0xff000000) >> 24);
  buf[1] = (unsigned char ) ((ip & 0xff0000) >> 16);
  buf[2] = (unsigned char ) ((ip & 0xff00) >> 8);
  buf[3] = (unsigned char ) ((ip & 0xff));  
  buf[4] = (unsigned char ) ((timestamp	& 0xff000000) >> 24);
  buf[5] = (unsigned char ) ((timestamp	& 0xff0000) >> 16);
  buf[6] = (unsigned char ) ((timestamp	& 0xff00) >> 8);
  buf[7] = (unsigned char ) ((timestamp	& 0xff));  
  len = 8;
  
  /* Append remaining components to buf */
  strcpy((char *)&buf[len], secret);
  len += strlen(secret);
  strcpy((char *)&buf[len], uid);
  len += strlen(uid);
  buf[len++] = 0;
  strcpy((char *)&buf[len], tokens);
  len += strlen(tokens);
  buf[len++] = 0;
  strcpy((char *)&buf[len], user_data);
  len += strlen(user_data);
  buf[len] = 0;

  /* Generate the initial digest */
  digest = ap_md5_binary(r->pool, buf, len);
  if (conf->debug >= 2) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT ticket_digest: digest0: '%s' (input length %d)", digest, len);
  }

  /* Copy digest + secret into buf2 */
  len = MD5_DIGEST_SZ + strlen(secret);
  memcpy(buf2, digest, MD5_DIGEST_SZ);
  memcpy(&buf2[MD5_DIGEST_SZ], secret, len - MD5_DIGEST_SZ);

  /* Generate the second digest */
  digest = ap_md5_binary(r->pool, buf2, len);
  if (conf->debug >= 2) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT ticket_digest: digest: '%s'", digest);
  }

  /* Should be noop, but just in case ... */
  if (strlen(digest) > MD5_DIGEST_SZ) digest[MD5_DIGEST_SZ] = 0;

  return (digest);
}

/* Check if this is a parseable and valid (MD5) ticket
 * Returns 1 if valid, and the parsed ticket in parsed, 0 if not */
static int
valid_ticket(request_rec *r, const char *source, char *ticket, auth_tkt *parsed)
{
  char *digest;
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);

  /* Attempt to parse cookie */
  if (! parse_ticket(r, &ticket, parsed)) {
    if (conf->debug >= 1) {
      ap_log_rerror(APLOG_MARK, APLOG_WARNING, APR_SUCCESS, r, 
        "TKT valid_ticket: unparseable %s ticket found ('%s')", source, ticket);
    }
    return 0;
  }

  if (conf->debug >= 1) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT valid_ticket: (parsed) uid '%s', tokens '%s', user_data '%s', ts '%d'", 
      parsed->uid, parsed->tokens, parsed->user_data, parsed->timestamp);
  }

  /* Check MD5 hash */
  digest = ticket_digest(r, parsed, 0);
  if (memcmp(ticket, digest, MD5_DIGEST_SZ) != 0) {
    ap_log_rerror(APLOG_MARK, APLOG_WARNING, APR_SUCCESS, r, 
      "TKT valid_ticket: ticket found, but hash is invalid - digest '%s', ticket '%32.32s'", digest, ticket);
    return 0;
  }

  return 1;
}

/* Check for required auth tokens 
 * Returns 1 on success, 0 on failure */
static int
check_tokens(request_rec *r, char *tokens)
{
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);
  char *next_parsed_token;
  const char *t = NULL;
  int match = 0; 

  /* Success if no tokens required */
  if (conf->auth_token->nelts == 0 || 
      strcmp(((char **) conf->auth_token->elts)[0], "NULL") == 0) {
    return 1;
  }
  /* Failure if required and no user tokens found */
  if (tokens == NULL || strlen(tokens) == 0) {
    return 0;
  }

  t = apr_pstrdup(r->pool, tokens); 
  
  while (*t && (next_parsed_token = ap_getword(r->pool, &t, ','))) {
    char ** auth_tokens = (char **) conf->auth_token->elts;
    int i;

    for (i=0; i < conf->auth_token->nelts; i++) {
      int token_len = strlen(auth_tokens[i]);
      if (strncmp(auth_tokens[i], next_parsed_token, token_len) == 0 &&
	  next_parsed_token[token_len] == 0) {
	match = 1;
	break;
      }
    }
    if (match) break;
  }

  if (conf->debug >= 1 && ! match) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT: no matching tokens! (user tokens '%s')", tokens);
  }

  return match;
}

/* Refresh the auth cookie if timeout refresh is set */
static void
refresh_cookie(request_rec *r, auth_tkt *parsed, int timeout, int force_flag)
{
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);

  /* The timeout refresh is a double between 0 and 1, signifying what
   * proportion of the timeout should be left before we refresh i.e. 
   * 0 means never refresh (hard timeouts); 1 means always refresh;
   * .33 means only refresh if less than a third of the timeout 
   * period remains. */ 
  unsigned int now = time(NULL);
  int remainder = parsed->timestamp + timeout - now;
  double refresh_sec = conf->timeout_refresh * timeout;

  if (conf->debug >= 1) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT: timeout %d, refresh %.3f, remainder %d, refresh_sec %.3f",
	timeout, conf->timeout_refresh, remainder, refresh_sec);
  }

  /* If less than our refresh_sec threshold, freshen the cookie */
  if (force_flag || remainder < refresh_sec) {
    char *ticket, *ticket_base64;
    char *digest = ticket_digest(r, parsed, now);
    if (parsed->tokens) {
      ticket = apr_psprintf(r->pool,
        "%s%08x%s%c%s%c%s", 
          digest, now, parsed->uid, 
	  SEPARATOR, parsed->tokens, 
	  SEPARATOR, parsed->user_data);
    }
    else {
      ticket = apr_psprintf(r->pool,
        "%s%08x%s%c%s", 
          digest, now, parsed->uid, SEPARATOR, parsed->user_data);
    }
    ticket_base64 = ap_pbase64encode(r->pool, ticket);

    send_auth_cookie(r, ticket_base64); 
  }
}
  
/* Check whether the given timestamp has timed out 
 * Returns 1 if okay, 0 if timed out */
static int
check_timeout(request_rec *r, auth_tkt *parsed)
{
  char *timeout_date, *timeout_cookie;
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);
  unsigned int now = time(NULL);
  unsigned int timestamp = parsed->timestamp;
  char *domain = get_domain(r,conf);
  char *secure_cookie = conf->secure_cookie > 0 ? "; secure" : "";
  int timeout;

  /* Return OK if no timeout configured */
  if (conf->timeout_sec == 0) return 1;
  timeout = conf->timeout_sec == -1 ? DEFAULT_TIMEOUT_SEC : conf->timeout_sec;

  /* Check whether timestamp is still fresh */
  if (timestamp + timeout >= now) {
    if (conf->debug >= 1) {
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
        "TKT: cookie timeout still good: now %d, timeout: %d, tstamp: %d", 
        now, timeout, timestamp);
    }

    /* Check whether to refresh the cookie */
    if (conf->timeout_refresh > 0) 
      refresh_cookie(r, parsed, timeout, CHECK_REFRESH);

    return 1;
  }

  if (conf->debug >= 1) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT: ticket timed out: now %d, timeout: %d, tstamp: %d", 
      now, timeout, timestamp);
  }
    
  /* Delete cookie (set expired) if invalid, in case we want to set from url */
  timeout_date = ap_ht_time(r->pool, now - 3600, "%a %d %b %Y %T %Z", 0);
  timeout_cookie = domain ?
    apr_psprintf(r->pool,
      "%s=; path=/; domain=%s; expires=%s%s",
      conf->auth_cookie_name, domain, timeout_date, secure_cookie) :
    apr_psprintf(r->pool,
      "%s=; path=/; expires=%s%s",
      conf->auth_cookie_name, timeout_date, secure_cookie);
  apr_table_setn(r->err_headers_out, "Set-Cookie", timeout_cookie);

  return 0;
}

/* Strip specified query args from a url */
static char * 
query_strip(request_rec *r, const char *strip)
{
  const char *args = NULL;  /* url arguments string  */
  const char *key, *val;
  char *new_args = "";
  char *p;

  /* Use main request args if subrequest */
  request_rec *r_main = r->main == NULL ? r : r->main;
  if (r_main->args == NULL || strip == NULL) 
    return NULL;

  args = apr_pstrdup(r->pool, r_main->args); 

  /* Convert all '&' to ';' */
  while ((p = ap_strchr(args, '&')))
    *p = ';';

  /* Split into individual args */
  while (*args && (val = ap_getword(r->pool, &args, ';'))) {
    key = ap_getword(r->pool, &val, '=');

    /* Add to new_args only if key != strip */
    if (strlen(strip) != strlen(key) || strncmp(key,strip,strlen(strip)) != 0) 
      new_args = apr_psprintf(r->pool, "%s&%s=%s", new_args, key, val);
  }

  if (strlen(new_args) > 0)
    return new_args + 1;

  return NULL;
}

/* Hex conversion, from httpd util.c */
static const char c2x_table[] = "0123456789abcdef";
static APR_INLINE unsigned char *
c2x(unsigned what, unsigned char *where)
{
#if APR_CHARSET_EBCDIC
  what = apr_xlate_conv_byte(ap_hdrs_to_ascii, (unsigned char)what);
#endif /*APR_CHARSET_EBCDIC*/
  *where++ = '%';
  *where++ = c2x_table[what >> 4];
  *where++ = c2x_table[what & 0xf];
  return where;
}

/* Extra escaping - variant of httpd util.c ap_escape_path_segment */
static char * 
escape_extras(apr_pool_t *p, const char *segment)
{
  char *copy = apr_palloc(p, 3 * strlen(segment) + 1);
  const unsigned char *s = (const unsigned char *)segment;
  unsigned char *d = (unsigned char *)copy;
  unsigned c;
  
  while ((c = *s)) {
    if (c == '=' || c == '&' || c == ':') {
      d = c2x(c, d);
    }
    else {
      *d++ = c;
    }
    ++s;
  }
  *d = '\0';
  return copy;
}

/* External redirect to the given url, setting back cookie or arg */
static int 
redirect(request_rec *r, char *location)
{
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);

  char *domain = get_domain(r,conf);
  char *back_cookie_name = conf->back_cookie_name;
  char *back_arg_name = conf->back_arg_name;
  char *url, *cookie, *back;
  const char *hostinfo = 0;
  int port;

  /* Get the scheme we use (http or https) */
  const char *scheme = ap_http_method(r);

  /* Strip any auth_cookie_name arguments from the current args */
  char *query = query_strip(r, conf->auth_cookie_name);
  if (query == NULL) {
    query = "";
  }
  else if (strlen(query) > 0) {
    query = apr_psprintf(r->pool, "?%s", query);
  }

  /* Build back URL */
  /* Use Host header for host:port info if available */
  hostinfo = apr_table_get(r->headers_in, "Host");
  if (! hostinfo) {
    /* Fallback to using r->hostname and the server port. This usually
       works, but behind a reverse proxy the port may well be wrong. 
       On the other hand, it's really the proxy's problem, not ours.
    */
    ap_log_rerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS, r, 
      "TKT: could not find Host header, falling back to hostname/server port");
    port = ap_get_server_port(r);
    hostinfo = port == apr_uri_default_port_for_scheme(scheme) ?
      apr_psprintf(r->pool, "%s", r->hostname) :
      apr_psprintf(r->pool, "%s:%d", r->hostname, port);
  }
  back = apr_psprintf(r->pool, "%s://%s%s%s", 
    scheme, hostinfo, r->uri, query);

  if (conf->debug >= 1) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT: back url '%s'", back);
  }
  
  /* Escape testing */
  back = ap_escape_path_segment(r->pool, back);
  back = escape_extras(r->pool, back);

  /* Set back cookie if name is not null */
  if (back_cookie_name) {
    cookie = domain ?
      apr_psprintf(r->pool, "%s=%s; path=/; domain=%s", 
        back_cookie_name, back, domain) :
      apr_psprintf(r->pool, "%s=%s; path=/", 
        back_cookie_name, back);

    apr_table_setn(r->err_headers_out, "Set-Cookie", cookie);
    url = location;
  }

  /* If back_cookie_name not set, add a back url argument to url */
  else {
    char sep = ap_strchr(location, '?') ? ';' : '?';
    url = apr_psprintf(r->pool, "%s%c%s=%s", 
      location, sep, back_arg_name, back);
  }

  if (conf->debug >= 2) {
    ap_log_rerror(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, r, 
      "TKT: redirect '%s'", url);
  }
  apr_table_setn(r->headers_out, "Location", url);
  
  return HTTP_TEMPORARY_REDIRECT;
}

/* ----------------------------------------------------------------------- */
/* Debug routines */
void 
dump_config(request_rec *r) 
{
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);
  if (conf->debug >= 3) {
    auth_tkt_serv_conf *sconf =
      ap_get_module_config(r->server->module_config, &auth_tkt_module);

    /* Dump config settings */
    fprintf(stderr,"[ mod_auth_tkt config ]\n");
    fprintf(stderr,"URI: %s\n", r->uri);
    fprintf(stderr,"Filename: %s\n",                    r->filename);
    fprintf(stderr,"TKTAuthSecret: %s\n", 	        sconf->secret);
    fprintf(stderr,"directory: %s\n", 		        conf->directory);
    fprintf(stderr,"TKTAuthLoginURL: %s\n", 	        conf->login_url);
    fprintf(stderr,"TKTAuthTimeoutURL: %s\n", 	        conf->timeout_url);
    fprintf(stderr,"TKTAuthPostTimeoutURL: %s\n",	conf->post_timeout_url);
    fprintf(stderr,"TKTAuthUnauthURL: %s\n", 	        conf->unauth_url);
    fprintf(stderr,"TKTAuthCookieName: %s\n", 	        conf->auth_cookie_name);
    fprintf(stderr,"TKTAuthDomain: %s\n", 	        conf->auth_domain);
    fprintf(stderr,"TKTAuthCookieExpires: %d\n", 	conf->cookie_expires);
    fprintf(stderr,"TKTAuthBackCookieName: %s\n",	conf->back_cookie_name);
    fprintf(stderr,"TKTAuthBackArgName: %s\n",	        conf->back_arg_name);
    fprintf(stderr,"TKTAuthIgnoreIP: %d\n",	        conf->ignore_ip);
    fprintf(stderr,"TKTAuthRequireSSL: %d\n", 	        conf->require_ssl);
    fprintf(stderr,"TKTAuthCookieSecure: %d\n", 	conf->secure_cookie);
    fprintf(stderr,"TKTAuthTimeoutMin: %d\n", 	        conf->timeout_sec);
    fprintf(stderr,"TKTAuthTimeoutRefresh: %f\n",	conf->timeout_refresh);
    fprintf(stderr,"TKTAuthGuestLogin: %d\n",           conf->guest_login);
    fprintf(stderr,"TKTAuthGuestCookie: %d\n",          conf->guest_cookie);
    fprintf(stderr,"TKTAuthGuestUser: %s\n",            conf->guest_user);
    if (conf->auth_token->nelts > 0) {
      char ** auth_token = (char **) conf->auth_token->elts;
      int i;
      for (i = 0; i < conf->auth_token->nelts; i++) {
        fprintf(stderr, "TKTAuthToken: %s\n", auth_token[i]);
      }
    }
    fprintf(stderr,"TKTAuthDebug: %d\n",                conf->debug);
    fflush(stderr);
  }
}

/* ----------------------------------------------------------------------- */
/* Main ticket authentication */
static int
auth_tkt_check(request_rec *r)
{
  char *ticket;
  auth_tkt *parsed = apr_palloc(r->pool, sizeof(*parsed));
  auth_tkt_dir_conf *conf = 
    ap_get_module_config(r->per_dir_config, &auth_tkt_module);
  auth_tkt_serv_conf *sconf =
    ap_get_module_config(r->server->module_config, &auth_tkt_module);
  const char *scheme = ap_http_method(r);
  int guest = 0;
  int timeout;
  char *url = NULL;
#ifndef APACHE13
  char *guest_user;
  int guest_user_length;
  apr_uuid_t *uuid;
  char *uuid_str, *uuid_length_str;
  regex_t *uuid_regex;
  regmatch_t regm[UUID_SUBS];
  int uuid_length = -1;
  char *uuid_pre, *uuid_post;
#endif

  dump_config(r);

  /* Module not configured unless login_url or guest_login is set */
  if (! conf->login_url && conf->guest_login <= 0) {
    return DECLINED;
  }
  /* Module misconfigured unless secret set */
  if (! sconf->secret) {
    ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_SUCCESS, r, 
      "TKT: TKTAuthSecret missing");
    return HTTP_INTERNAL_SERVER_ERROR;
  }
  /* Redirect/login if scheme not "https" and require_ssl is set */
  if (conf->require_ssl > 0 && strcmp(scheme,"https") != 0) {
    ap_log_rerror(APLOG_MARK, APLOG_WARNING, APR_SUCCESS, r, 
      "TKT: redirect/login - unsecured request, TKTAuthRequireSSL is on");
    return redirect(r, conf->login_url);
  }
  /* Backwards compatibility mode for TKTAuthRequireSSL */
  if (conf->require_ssl > 0 && conf->secure_cookie == -1) {
    /* Set secure_cookie flag if require_ssl is set and secure_cookie is 
       undefined (as opposed to 'off') */
    ap_log_rerror(APLOG_MARK, APLOG_WARNING, APR_SUCCESS, r, 
      "TKT: TKTAuthRequireSSL on, but no TKTAuthCookieSecure found - "
      "please set TKTAuthCookieSecure explicitly, assuming 'on'");
    conf->secure_cookie = 1;
  }

  /* Check for url ticket - either found (accept) or empty (reset/login) */
  ticket = get_url_ticket(r);
  if (! ticket || ! valid_ticket(r, "url", ticket, parsed)) {
    ticket = get_cookie_ticket(r);
    if (! ticket || ! valid_ticket(r, "cookie", ticket, parsed)) {
      if (conf->guest_login > 0) {
        guest = 1;
        if (conf->guest_user) {
#ifdef APACHE13
          /* We don't support %U under apache1 at this point */
          parsed->uid = conf->guest_user;
#else
          guest_user = apr_pstrdup(r->pool, conf->guest_user);
          uuid_regex = ap_pregcomp(r->pool, "%([0-9]*)U", 0);
          if (!ap_regexec(uuid_regex, guest_user, UUID_SUBS, regm, 0)) {
            /* Check whether a UUID length was specified */
            if (regm[1].rm_so != -1) {
              uuid_length_str = ap_pregsub(r->pool, "$1", guest_user,
                UUID_SUBS, regm);
              if (uuid_length_str) 
                uuid_length = atoi(uuid_length_str);
            }
            if (uuid_length <= 0 || uuid_length > APR_UUID_FORMATTED_LENGTH) {
              uuid_length = APR_UUID_FORMATTED_LENGTH;
            }
            if (conf->debug >= 1) {
              ap_log_rerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS, r, 
                "TKT: %%U found in guest user (length %d)", uuid_length);
            }
            /* Generate the UUID */
            uuid = apr_palloc(r->pool, sizeof(*uuid));
            uuid_str = apr_palloc(r->pool, APR_UUID_FORMATTED_LENGTH + 1);
            apr_uuid_get(uuid);
            apr_uuid_format(uuid_str, uuid);
            if (uuid_length < APR_UUID_FORMATTED_LENGTH) 
              uuid_str[uuid_length] = '\0';
            /* Generate the new guest_user string */
            guest_user_length = strlen(guest_user);
            if (regm[0].rm_so > 1) {
              guest_user[regm[1].rm_so-1] = '\0';
              uuid_pre = guest_user;
            }
            else
              uuid_pre = "";
            if (regm[0].rm_eo < guest_user_length)
              uuid_post = guest_user + regm[0].rm_eo;
            else
              uuid_post = "";
            parsed->uid = apr_psprintf(r->pool, "%s%s%s", 
              uuid_pre, uuid_str, uuid_post);
          }
          else {
            parsed->uid = conf->guest_user;
          }
          /* With UUIDs, TKTAuthGuestCookie defaults to 'on' */
          if (conf->guest_cookie == -1) {
            conf->guest_cookie = 1;
          }
#endif
        }
        else {
          parsed->uid = DEFAULT_GUEST_USER;
        }
        parsed->user_data = "";
        parsed->tokens = "";
        ap_log_rerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS, r, 
          "TKT: no valid ticket found - accepting as guest user '%s'", 
            parsed->uid);
      } else {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS, r, 
          "TKT: no valid ticket found - redirecting to login url");
        return redirect(r, conf->login_url);
      }
    }
  }

  /* Valid ticket, check tokens - redirect/unauthorised if so */
  if (! check_tokens(r, parsed->tokens)) {
    return redirect(r, conf->unauth_url ? conf->unauth_url : conf->login_url);
  }

  /* Valid ticket, check timeout - redirect/timed-out if so */
  if (! guest && ! check_timeout(r, parsed)) {
    /* Special timeout URL can be defined for POST requests */
    if (strcmp(r->method, "POST") == 0 && conf->post_timeout_url) {
      url = conf->post_timeout_url;
    }
    else {
      url = conf->timeout_url ? conf->timeout_url : conf->login_url;
    }
    return redirect(r, url);
  }

  /* If a new guest login and the guest_cookie flag is set, force a cookie refresh */
  if (guest && conf->guest_cookie > 0) {
    timeout = conf->timeout_sec == -1 ? DEFAULT_TIMEOUT_SEC : conf->timeout_sec;
    refresh_cookie(r, parsed, timeout, FORCE_REFRESH);
  }

  /* Setup apache user, auth_type, and environment variables */
#ifdef APACHE13
  r->connection->user = parsed->uid;
  r->connection->ap_auth_type = "Basic";
#else
  r->user = parsed->uid;
  r->ap_auth_type = "Basic";
#endif
  apr_table_set(r->subprocess_env, REMOTE_USER_ENV,        parsed->uid);
  apr_table_set(r->subprocess_env, REMOTE_USER_DATA_ENV,   parsed->user_data);
  apr_table_set(r->subprocess_env, REMOTE_USER_TOKENS_ENV, parsed->tokens);

  return OK;
}

/* ----------------------------------------------------------------------- */
/* Setup main module data structure */

#ifdef APACHE13
/* Apache 1.3 style */

module MODULE_VAR_EXPORT auth_tkt_module = {
  STANDARD_MODULE_STUFF, 
  auth_tkt_version,             /* initializer */
  create_auth_tkt_config,	/* create per-dir    config structures */
  merge_auth_tkt_config,	/* merge  per-dir    config structures */
  create_auth_tkt_serv_config,	/* create per-server config structures */
  merge_auth_tkt_serv_config,   /* merge  per-server config structures */
  auth_tkt_cmds,                /* table of config file commands       */
  NULL,                         /* handlers */
  NULL,                         /* filename translation */
  auth_tkt_check,               /* check user_id */
  NULL,                         /* check auth */
  NULL,                         /* check access */
  NULL,                         /* type_checker */
  NULL,                         /* fixups */
  NULL,                         /* logger */
  NULL,                         /* header parser */
  NULL,                         /* chitkt_init */
  NULL,                         /* chitkt_exit */
  NULL                          /* post read-request */
};

#else
/* Apache 2.0 style */

/* Register hooks */
static void
auth_tkt_register_hooks (apr_pool_t *p)
{
  ap_hook_post_config(auth_tkt_version, NULL, NULL, APR_HOOK_MIDDLE);
  ap_hook_check_user_id(auth_tkt_check, NULL, NULL, APR_HOOK_FIRST);
}

/* Declare and populate the main module data structure */
module AP_MODULE_DECLARE_DATA auth_tkt_module = {
  STANDARD20_MODULE_STUFF, 
  create_auth_tkt_config,	/* create per-dir    config structures */
  merge_auth_tkt_config,	/* merge  per-dir    config structures */
  create_auth_tkt_serv_config,	/* create per-server config structures */
  merge_auth_tkt_serv_config,   /* merge  per-server config structures */
  auth_tkt_cmds,                /* table of config file commands       */
  auth_tkt_register_hooks       /* register hooks                      */
};

#endif

/* 
 * arch-tag: 70090400-e89e-46e1-9c78-1a9ad0cd47eb
 * vim:sw=2:sm
 */