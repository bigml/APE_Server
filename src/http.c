/*
  Copyright (C) 2006, 2007, 2008, 2009, 2010, 2011  Anthony Catel <a.catel@weelya.com>

  This file is part of APE Server.
  APE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  APE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with APE ; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

/* http.c */

#include <string.h>

#include "http.h"
#include "sock.h"
#include "main.h"
#include "utils.h"
#include "dns.h"
#include "log.h"
#include <stdlib.h> /* endian macros */
#include <arpa/inet.h>

#if !defined(_BIG_ENDIAN) && !defined(_LITTLE_ENDIAN)
# if defined(__i386) || defined(__amd64) || defined(__arm)
#  define _LITTLE_ENDIAN 1
# else
#  if defined(__sparc) || defined(__mips)
#   define _BIG_ENDIAN 1
#  endif
# endif
#endif

#if !defined(_BIG_ENDIAN) && !defined(_LITTLE_ENDIAN)
# error "Unable to determine platform endianness"
#endif

#define HTTP_PREFIX		"http://"
#define WS_MAGIC_VALUE 0x00003600

struct _http_attach {
	char host[1024];
	char file[1024];
	
	const char *post;
	unsigned short int port;
};

static struct _http_header_line *parse_header_line(const char *line)
{
	unsigned int i;
	unsigned short int state = 0;
	struct _http_header_line *hline = NULL;
	for (i = 0; i < 1024 && line[i] != '\0' && line[i] != '\r' && line[i] != '\n'; i++) {
		if (i == 0) {
			hline = xmalloc(sizeof(*hline));
			hline->key.len = 0;
			hline->value.len = 0;
		}
		switch(state) {
			case 0:
				if ((i == 0 && (line[i] == ':' || line[i] == ' ')) || (line[i] == ':' && line[i+1] != ' ') || (i > 63)) {
					free(hline);
					return NULL;
				}
				if (line[i] == ':') {
					hline->key.val[hline->key.len] = '\0';
					state = 1;
					i++;
				} else {
					hline->key.val[hline->key.len++] = line[i];
				}
				break;
			case 1:
				hline->value.val[hline->value.len++] = line[i];
				break;
		}
	}
	if (!state) {
		free(hline);
		return NULL;
	}
	hline->value.val[hline->value.len] = '\0';

	return hline;
}

char *get_header_line(struct _http_header_line *lines, const char *key)
{
	while (lines != NULL) {
		if (strcasecmp(lines->key.val, key) == 0) {
			return lines->value.val;
		}
		
		lines = lines->next;
	}
	
	return NULL;
}

static void process_websocket_frame(ape_socket *co, acetables *g_ape)
{
    ape_buffer *buffer = &co->buffer_in;
    websocket_state *websocket = co->parser.data;
    ape_parser *parser = &co->parser;
    
    unsigned char *pData;
    
    for (pData = (unsigned char *)&buffer->data[websocket->offset]; websocket->offset < buffer->length; websocket->offset++, pData++) {
        switch(websocket->step) {
            case WS_STEP_KEY:
                /* Copy the xor key (32 bits) */
                websocket->key.val[websocket->key.pos] = *pData;
                if (++websocket->key.pos == 4) {
                    websocket->step = WS_STEP_DATA;
                }
                break;
            case WS_STEP_START:
                /* Contain fragmentaiton infos & opcode (+ reserved bits) */
                websocket->frame_payload.start = *pData;

                websocket->step = WS_STEP_LENGTH;
                break;
            case WS_STEP_LENGTH:
                /* Check for MASK bit */
                if (!(*pData & 0x80)) {
                    return;
                }
                switch (*pData & 0x7F) { /* 7bit length */
                    case 126:
                        /* Following 16bit are length */
                        websocket->step = WS_STEP_SHORT_LENGTH;
                        break;
                    case 127:
                        /* Following 64bit are length */
                        websocket->step = WS_STEP_EXTENDED_LENGTH;
                        break;
                    default:
                        /* We have the actual length */
                        websocket->frame_payload.extended_length = *pData & 0x7F;
                        websocket->step = WS_STEP_KEY;
                        break;
                }
                break;
            case WS_STEP_SHORT_LENGTH:
                memcpy(((char *)&websocket->frame_payload)+(websocket->frame_pos), 
                        pData, 1);
                if (websocket->frame_pos == 3) {
                    websocket->frame_payload.extended_length = ntohs(websocket->frame_payload.short_length);
                    websocket->step = WS_STEP_KEY;
                }
                break;
            case WS_STEP_EXTENDED_LENGTH:
                memcpy(((char *)&websocket->frame_payload)+(websocket->frame_pos),
                        pData, 1);
                if (websocket->frame_pos == 9) {
                    websocket->frame_payload.extended_length = ntohl(websocket->frame_payload.extended_length >> 32);
                    websocket->step = WS_STEP_KEY;
                }        
                break;
            case WS_STEP_DATA:
                if (websocket->data_pos == 0) {
                    websocket->data_pos = websocket->offset;
                }
                
                *pData ^= websocket->key.val[(websocket->frame_pos - websocket->data_pos) % 4];

                if (--websocket->frame_payload.extended_length == 0) {
                    unsigned char saved;
                    
                    websocket->data = &buffer->data[websocket->data_pos];
                    websocket->step = WS_STEP_START;
                    websocket->frame_pos = -1;
                    websocket->frame_payload.extended_length = 0;
                    websocket->data_pos = 0;
                    websocket->key.pos = 0;

                    switch(websocket->frame_payload.start & 0x0F) {
                        case 0x8:
                        {
                            /*
                              Close frame
                              Reply by a close response
                            */
                            char payload_head[2] = { 0x88, 0x00 };
                            sendbin(co->fd, payload_head, 2, 0, g_ape);
                            return;
                        }
                        case 0x9:
                        {
                            int body_length = &buffer->data[websocket->offset+1] - websocket->data;
                            char payload_head[2] = { 0x8a, body_length & 0x7F };
                            
                            /* All control frames MUST be 125 bytes or less */
                            if (body_length > 125) {
                                payload_head[0] = 0x88;
                                payload_head[1] = 0x00;      
                                sendbin(co->fd, payload_head, 2, 1, g_ape);
                                return;
                            }
                            PACK_TCP(co->fd);
                            sendbin(co->fd, payload_head, 2, 0, g_ape);
                            if (body_length) {
                                sendbin(co->fd, websocket->data, body_length, 0, g_ape);
                            }
                            FLUSH_TCP(co->fd);
                            break;
                        }
                        case 0xA: /* Never called as long as we never ask for pong */
                            break;
                        default:
                            /* Data frame */
                            saved = buffer->data[websocket->offset+1];
                            buffer->data[websocket->offset+1] = '\0';
                            parser->onready(parser, g_ape);
                            buffer->data[websocket->offset+1] = saved;                            
                            break;
                    }
                    
                    if (websocket->offset+1 == buffer->length) {
                        websocket->offset = 0;
                        buffer->length = 0;
                        websocket->frame_pos = 0;
                        websocket->key.pos = 0;
                        return;
                    }
                }
                break;
            default:
                break;
        }
        websocket->frame_pos++;
    }
}

static void process_websocket_frame_06(ape_socket *co, acetables *g_ape)
{
    ape_buffer *buffer = &co->buffer_in;
    websocket_state *websocket = co->parser.data;
    ape_parser *parser = &co->parser;
    
    unsigned char *pData;
    
    for (pData = (unsigned char *)&buffer->data[websocket->offset]; websocket->offset < buffer->length; websocket->offset++, pData++) {
        if (websocket->step != WS_STEP_KEY) {
            /* de-cypher the data */
            *pData ^= websocket->key.val[(websocket->frame_pos-4) % 4];
        }
        switch(websocket->step) {
            case WS_STEP_KEY:
                /* Copy the xor key (32 bits) */
                websocket->key.val[websocket->frame_pos] = *pData;
                if (websocket->frame_pos == 3) {
                    websocket->step = WS_STEP_START;
                }
                break;
            case WS_STEP_START:
                /* Contain fragmentaiton infos & opcode (+ reserved bits) */
                websocket->frame_payload.start = *pData;
                websocket->step = WS_STEP_LENGTH;
                break;
            case WS_STEP_LENGTH: /* frame 5 */
                switch (*pData & 0x7F) { /* 7bit length */
                    case 126:
                        /* Following 16bit are length */
                        websocket->step = WS_STEP_SHORT_LENGTH;
                        break;
                    case 127:
                        /* Following 64bit are length */
                        websocket->step = WS_STEP_EXTENDED_LENGTH;
                        break;
                    default:
                        /* We have the actual length */
                        websocket->frame_payload.extended_length = *pData & 0x7F;
                        websocket->step = WS_STEP_DATA;
                        break;
                }
                break;
            case WS_STEP_SHORT_LENGTH: /* frame 6-7 */
                memcpy(((char *)&websocket->frame_payload)+(websocket->frame_pos-4), 
                        pData, 1);
                if (websocket->frame_pos == 7) {
                    websocket->frame_payload.extended_length = ntohs(websocket->frame_payload.short_length);
                    websocket->step = WS_STEP_DATA;
                }
                break;
            case WS_STEP_EXTENDED_LENGTH: /* frame 6-7-8-9-10-11-12-13 */
                memcpy(((char *)&websocket->frame_payload)+(websocket->frame_pos-4),
                        pData, 1);
                if (websocket->frame_pos == 13) {
                    websocket->frame_payload.extended_length = ntohl(websocket->frame_payload.extended_length >> 32);
                    websocket->step = WS_STEP_DATA;
                }        
                break;
            case WS_STEP_DATA:
                if (websocket->data_pos == 0) {
                    websocket->data_pos = websocket->offset;
                }
                if (--websocket->frame_payload.extended_length == 0) {
                    unsigned char saved;
                    
                    websocket->data = &buffer->data[websocket->data_pos];
                    websocket->step = WS_STEP_KEY;
                    websocket->frame_pos = -1;
                    websocket->frame_payload.extended_length = 0;
                    websocket->data_pos = 0;
                    
                    switch(websocket->frame_payload.start & 0x0F) {
                        case 0x01:
                        {
                            /*
                              Close frame
                              Reply by a close response
                            */
                            char payload_head[2] = { 0x81, 0x00 };                    
                            sendbin(co->fd, payload_head, 2, 0, g_ape);
                            return;
                        }
                        case 0x02:
                        {
                            int body_length = &buffer->data[websocket->offset+1] - websocket->data;
                            char payload_head[2] = { 0x83, body_length & 0x7F };
                            
                            /* All control frames MUST be 125 bytes or less */
                            if (body_length > 125) {
                                payload_head[0] = 0x81;  
                                payload_head[1] = 0x00;      
                                sendbin(co->fd, payload_head, 2, 1, g_ape);
                                return;
                            }
                            PACK_TCP(co->fd);
                            sendbin(co->fd, payload_head, 2, 0, g_ape);
                            if (body_length) {
                                sendbin(co->fd, websocket->data, body_length, 0, g_ape);
                            }
                            FLUSH_TCP(co->fd);
                            break;
                        }
                        case 0x03: /* Never called as long as we never ask for pong */
                            break;
                        default:
                            /* Data frame */
                            saved = buffer->data[websocket->offset+1];
                            buffer->data[websocket->offset+1] = '\0';

                            parser->onready(parser, g_ape);
                            buffer->data[websocket->offset+1] = saved;                            
                            break;
                    }
                    
                    if (websocket->offset+1 == buffer->length) {
                        websocket->offset = 0;
                        buffer->length = 0;
                        websocket->frame_pos = 0;
                        return;
                    }
                }
                break;
            default:
                break;
        }
        websocket->frame_pos++;
    }
}


void process_websocket(ape_socket *co, acetables *g_ape)
{
	char *pData;
	ape_buffer *buffer = &co->buffer_in;
	websocket_state *websocket = co->parser.data;
	ape_parser *parser = &co->parser;
	
	char *data = pData = &buffer->data[websocket->offset];

	if (buffer->length == 0 || parser->ready == 1) {
		return;
	}
	
	if (buffer->length > 502400) {
		shutdown(co->fd, 2);
		return;
	}
	
	if (websocket->version == WS_IETF_06) {
	    process_websocket_frame_06(co, g_ape);
	    return;
	}
	if (websocket->version == WS_IETF_07) {
	    process_websocket_frame(co, g_ape);
	    return;
	}	

	data[buffer->length - websocket->offset] = '\0';
    
	if (*data == '\0') {
		data = &data[1];
	}

	while(data++ != &buffer->data[buffer->length]) {
	
		if ((unsigned char)*data == 0xFF) {
			*data = '\0';
			
			websocket->data = &pData[1];
			parser->onready(parser, g_ape);

			websocket->offset += (data - pData)+1;
			
			if (websocket->offset == buffer->length) {
				parser->ready = -1;
				buffer->length = 0;
				websocket->offset = 0;
				
				return;
			}
			
			break;
		}
	}
	
	if (websocket->offset != buffer->length && data != &buffer->data[buffer->length+1]) {
		process_websocket(co, g_ape);
	}
}

/* Just a lightweight http request processor */
void process_http(ape_socket *co, acetables *g_ape)
{
	ape_buffer *buffer = &co->buffer_in;
	http_state *http = co->parser.data;
	ape_parser *parser = &co->parser;
	
	char *data = buffer->data;
	int pos, read, p = 0;
	
	if (buffer->length == 0 || parser->ready == 1 || http->error == 1) {
		return;
	}

	/* 0 will be erased by the next read()'ing loop */
	data[buffer->length] = '\0';
	
	data = &data[http->pos];
	
	if (*data == '\0') {
		return;
	}
	
	/* Update the address of http->data and http->uri if buffer->data has changed (realloc) */
	if (http->buffer_addr != NULL && buffer->data != http->buffer_addr) {
		if (http->data != NULL) http->data = &buffer->data[(void *)http->data - (void *)http->buffer_addr];
		if (http->uri != NULL) http->uri = &buffer->data[(void *)http->uri - (void *)http->buffer_addr];
		http->buffer_addr = buffer->data;
	}
	
	switch(http->step) {
		case 0:
			pos = seof(data, '\n');
			if (pos == -1) {
				return;
			}
			
			switch(*(unsigned int *)data) {
#ifdef _LITTLE_ENDIAN
				case 0x20544547: /* GET + space */
#endif
#ifdef _BIG_ENDIAN
				case 0x47455420: /* GET + space */
#endif
					http->type = HTTP_GET;
					p = 4;
					break;
#ifdef _LITTLE_ENDIAN
				case 0x54534F50: /* POST */
#endif
#ifdef _BIG_ENDIAN
				case 0x504F5354: /* POST */
#endif
					http->type = HTTP_POST;
					p = 5;
					break;
				default:
					alog_info("Invalid HTTP method in request: %s", data);
					http->error = 1;
					shutdown(co->fd, 2);
					return;
			}
			
			if (data[p] != '/') {
				http->error = 1;
				shutdown(co->fd, 2);
				return;
			} else {
				int i = p;
				while (p++) {
					switch(data[p]) {
						case ' ':
							http->pos = pos;
							http->step = 1;
							http->uri = &data[i];
							http->buffer_addr = buffer->data;
							data[p] = '\0';
							process_http(co, g_ape);
							return;
						case '?':
							if (data[p+1] != ' ' && data[p+1] != '\r' && data[p+1] != '\n') {
								http->buffer_addr = buffer->data;
								http->data = &data[p+1];
							}
							break;
						case '\r':
						case '\n':
						case '\0':
							alog_info("Invalid line ending in request: %s", data);
							http->error = 1;
							shutdown(co->fd, 2);
							return;
					}
				}
			}
			break;
		case 1:
			pos = seof(data, '\n');
			if (pos == -1) {

				return;
			}
			if (pos == 1 || (pos == 2 && *data == '\r')) {
				if (http->type == HTTP_GET) {
					/* Ok, at this point we have a blank line. Ready for GET */
					buffer->data[http->pos] = '\0';
					urldecode(http->uri);
					parser->onready(parser, g_ape);
					parser->ready = -1;
					buffer->length = 0;
					return;
				} else if (http->type == HTTP_GET_WS) { /* WebSockets handshake needs to read 8 bytes */
					//urldecode(http->uri);
					http->contentlength = 8;
					http->buffer_addr = buffer->data;
					http->data = &buffer->data[http->pos+(pos)];
					http->step = 2;
				} else {
					/* Content-Length is mandatory in case of POST */
					if (http->contentlength == 0) {
						http->error = 1;
						shutdown(co->fd, 2);
						return;
					} else {
						http->buffer_addr = buffer->data; // save the addr
						http->data = &buffer->data[http->pos+(pos)];
						http->step = 2;
					}
				}
			} else {
				struct _http_header_line *hl;

				if ((hl = parse_header_line(data)) != NULL) {
					hl->next = http->hlines;
					http->hlines = hl;
					if (strcasecmp(hl->key.val, "host") == 0) {
						http->host = hl->value.val;
					}
				}
				if (http->type == HTTP_POST) {
					/* looking for content-length instruction */
					if (pos <= 25 && strncasecmp("content-length: ", data, 16) == 0) {
						int cl = atoi(&data[16]);

						/* Content-length can't be negative... */
						if (cl < 1 || cl > MAX_CONTENT_LENGTH) {
							http->error = 1;
							shutdown(co->fd, 2);
							return;
						}
						/* At this time we are ready to read "cl" bytes contents */
						http->contentlength = cl;

					}
				} else if (http->type == HTTP_GET) {
					if (strncasecmp("Sec-WebSocket-Key1: ", data, 20) == 0) {
						http->type = HTTP_GET_WS;
					}
				}
			}
			http->pos += pos;
			process_http(co, g_ape);
			break;
		case 2:
			read = buffer->length - http->pos; // data length
			http->pos += read;
			http->read += read;
			
			if (http->read >= http->contentlength) {

				parser->ready = 1;
				urldecode(http->uri);
				/* no more than content-length */
				buffer->data[http->pos - (http->read - http->contentlength)] = '\0';
				
				parser->onready(parser, g_ape);
				parser->ready = -1;
				buffer->length = 0;
			}
			break;
		default:
			break;
	}
}


/* taken from libevent */

int parse_uri(char *url, char *host, u_short *port, char *file)
{
	char *p;
	const char *p2;
	int len;

	len = strlen(HTTP_PREFIX);
	if (strncasecmp(url, HTTP_PREFIX, len)) {
		return -1;
	}

	url += len;

	/* We might overrun */
	strncpy(host, url, 1023);


	p = strchr(host, '/');
	if (p != NULL) {
		*p = '\0';
		p2 = p + 1;
	} else {
		p2 = NULL;
	}
	if (file != NULL) {
		/* Generate request file */
		if (p2 == NULL)
			p2 = "";
		sprintf(file, "/%s", p2);
	}

	p = strchr(host, ':');
	
	if (p != NULL) {
		*p = '\0';
		*port = atoi(p + 1);

		if (*port == 0)
			return -1;
	} else
		*port = 80;

	return 0;
}

http_headers_response *http_headers_init(int code, char *detail, int detail_len)
{
	http_headers_response *headers;
	
	if (detail_len > 63 || (code < 100 && code >= 600)) {
		return NULL;
	}
	
	headers = xmalloc(sizeof(*headers));

	headers->code = code;
	headers->detail.len = detail_len;
	memcpy(headers->detail.val, detail, detail_len + 1);
	
	headers->fields = NULL;
	headers->last = NULL;
	
	return headers;
}

void http_headers_set_field(http_headers_response *headers, const char *key, int keylen, const char *value, int valuelen)
{
	struct _http_headers_fields *field = NULL, *look_field;
	int value_l, key_l;

	value_l = (valuelen ? valuelen : strlen(value));
	key_l = (keylen ? keylen : strlen(key));
	
	if (key_l >= 32) {
		return;
	}
	
	for(look_field = headers->fields; look_field != NULL; look_field = look_field->next) {
		if (strncasecmp(look_field->key.val, key, key_l) == 0) {
			field = look_field;
			break;
		}
	}
	
	if (field == NULL) {
		field = xmalloc(sizeof(*field));
		field->next = NULL;
	
		if (headers->fields == NULL) {
			headers->fields = field;
		} else {
			headers->last->next = field;
		}
		headers->last = field;
	} else {
		free(field->value.val);
	}
	
	field->value.val = xmalloc(sizeof(char) * (value_l + 1));
	
	memcpy(field->key.val, key, key_l + 1);
	memcpy(field->value.val, value, value_l + 1);
	
	field->value.len = value_l;
	field->key.len = key_l;

}

/*
http_headers_response *headers = http_headers_init(200, "OK", 2);
http_headers_set_field(headers, "Content-Length", 0, "100", 0);
http_send_headers(headers, cget->client, g_ape);
*/

int http_send_headers(http_headers_response *headers, const char *default_h, unsigned int default_len, ape_socket *client, acetables *g_ape)
{
	char code[4];
	int finish = 1;
	struct _http_headers_fields *fields;
	//HTTP/1.1 200 OK\r\n
	
	if (headers == NULL) {
		finish &= sendbin(client->fd, (char *)default_h, default_len, 0, g_ape);
	} else {
		/* We have a lot of write syscall here. TODO : use of writev */
		itos(headers->code, code, 4);
		finish &= sendbin(client->fd, "HTTP/1.1 ", 9, 0, g_ape);
		finish &= sendbin(client->fd, code, 3, 0, g_ape);
		finish &= sendbin(client->fd, " ", 1, 0, g_ape);
		finish &= sendbin(client->fd, headers->detail.val, headers->detail.len, 0, g_ape);
		finish &= sendbin(client->fd, "\r\n", 2, 0, g_ape);
	
		for (fields = headers->fields; fields != NULL; fields = fields->next) {
			finish &= sendbin(client->fd, fields->key.val, fields->key.len, 0, g_ape);
			finish &= sendbin(client->fd, ": ", 2, 0, g_ape);
			finish &= sendbin(client->fd, fields->value.val, fields->value.len, 0, g_ape);
			finish &= sendbin(client->fd, "\r\n", 2, 0, g_ape);
		
			fields = fields->next;
		}
	
		finish &= sendbin(client->fd, "\r\n", 2, 0, g_ape);
	}
	
	return finish;
}

void http_headers_free(http_headers_response *headers)
{
	struct _http_headers_fields *fields;
	
	if (headers == NULL) {
		return;
	}
	
	fields = headers->fields;
	
	while(fields != NULL) {
		struct _http_headers_fields *tmpfields = fields->next;
		
		free(fields->value.val);
		
		free(fields);
		fields = tmpfields;
	}
	free(headers);
}

void free_header_line(struct _http_header_line *line)
{
	struct _http_header_line *tline;
	
	while (line != NULL) {
		tline = line->next;
		free(line);
		line = tline;
	}
}

