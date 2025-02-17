#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "log.h"
#include "hnpub.h"
#include "utils.h"

void hn_senderr(callbackp *callbacki, int code, char *msg)
{
    if (callbacki == NULL || msg == NULL)
        return;
    
    RAW *raw;
    json_item *ej = json_new_object();
    json_set_property_intZ(ej, "code", code);
    json_set_property_strZ(ej, "value", msg);
    raw = forge_raw(RAW_ERR, ej);
    send_raw_inline(callbacki->client, callbacki->transport, raw, callbacki->g_ape);
}

void hn_senderr_sub(callbackp *callbacki, int code, char *msg)
{
    if (callbacki == NULL || msg == NULL)
        return;
    
    RAW *raw;
    json_item *ej = json_new_object();
    json_set_property_intZ(ej, "code", code);
    json_set_property_strZ(ej, "value", msg);
    raw = forge_raw(RAW_ERR, ej);
	post_raw_sub(raw, callbacki->call_subuser, callbacki->g_ape);
	POSTRAW_DONE(raw);
}

void hn_senddata_sub(callbackp *callbacki, int code, char *msg)
{
    if (callbacki == NULL || msg == NULL)
        return;
    
    RAW *raw;
    json_item *ej = json_new_object();
    json_set_property_intZ(ej, "code", code);
    json_set_property_strZ(ej, "value", msg);
    raw = forge_raw(RAW_DATA, ej);
	post_raw_sub(raw, callbacki->call_subuser, callbacki->g_ape);
	POSTRAW_DONE(raw);
}

void hn_senddata(callbackp *callbacki, int code, char *msg)
{
    if (callbacki == NULL || msg == NULL)
        return;
    
    RAW *raw;
    json_item *ej = json_new_object();
    json_set_property_intZ(ej, "code", code);
    json_set_property_strZ(ej, "value", msg);
    raw = forge_raw(RAW_DATA, ej);
    send_raw_inline(callbacki->client, callbacki->transport, raw, callbacki->g_ape);
}

void hn_sendraw(callbackp *callbacki, char *rawname, char *msg)
{
    if (callbacki == NULL || rawname == NULL || msg == NULL)
        return;
    
    RAW *raw;
    json_item *ej = json_new_object();
    json_set_property_strZ(ej, "value", msg);
    raw = forge_raw(rawname, ej);
    send_raw_inline(callbacki->client, callbacki->transport, raw, callbacki->g_ape);
}

int hn_isvaliduin(char *uin)
{
	if (uin == NULL)
		return 0;
        
	char *p = uin;
	while (*p != '\0') {
		if (!isdigit((int)*p))
			return 0;
		p++;
	}
	return 1;
}

int hn_str_cmp(void *a, void *b)
{
	char *sa, *sb;
	sa = (char*)a;
	sb = (char*)b;
	
	return strcmp(sa, sb);
}

unsigned char *hn_unescape (unsigned char *s, int buflen, char esc_char)
{
  int i = 0, o = 0;

  if (s == NULL) return s;
  while (i < buflen)
  {
    if (s[i] == esc_char && (i+2 < buflen) &&
	isxdigit(s[i+1]) && isxdigit(s[i+2]))
    {
      unsigned char num;
      num = (s[i+1] >= 'A') ? ((s[i+1] & 0xdf) - 'A') + 10 : (s[i+1] - '0');
      num *= 16;
      num += (s[i+2] >= 'A') ? ((s[i+2] & 0xdf) - 'A') + 10 : (s[i+2] - '0');
      s[o++] = num;
      i+=3;
    }
    else {
      s[o++] = s[i++];
    }
  }
  if (i && o) s[o] = '\0';
  return s;
}


/*
 * anchor
 */
anchor* anchor_new(const char *name, const char *href,
				   const char *title, const char *target)
{
	if (!name || !href || !title || !target) return NULL;

	anchor *anc = xmalloc(sizeof(anchor));
	anc->name = strdup(name);
	anc->href = strdup(href);
	anc->title = strdup(title);
	anc->target = strdup(target);

	return anc;
}

void anchor_free(void *a)
{
	if (!a) return;

	anchor *anc = (anchor*)a;
	SFREE(anc->name);
	SFREE(anc->href);
	SFREE(anc->title);
	SFREE(anc->target);
	SFREE(anc);
}

int anchor_cmp(void *a, void *b)
{
	anchor *anca, *ancb;
	anca = (anchor*)a;
	ancb = (anchor*)b;

	return strcmp(anca->href, ancb->href);
}

#if 0
int hn_chatnum_fkq_cmp(void *a, void *b)
{
	HTBL_ITEM *sa, *sb;
	sa = (HTBL_ITEM*)a;
	sb = (HTBL_ITEM*)b;

	chatNum *ca, *cb;
	ca = (chatNum*)sa->addrs;
	cb = (chatNum*)sb->addrs;

	return cb->fkq - ca->fkq;
}

int hn_chatnum_friend_cmp(void *a, void *b)
{
	HTBL_ITEM *sa, *sb;
	sa = (HTBL_ITEM*)a;
	sb = (HTBL_ITEM*)b;

	chatNum *ca, *cb;
	ca = (chatNum*)sa->addrs;
	cb = (chatNum*)sb->addrs;

	return cb->friend - ca->friend;
}

#endif
