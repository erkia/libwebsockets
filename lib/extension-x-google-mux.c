#include "private-libwebsockets.h"
#include "extension-x-google-mux.h"

static int ongoing_subchannel;
static struct libwebsocket * tag_with_parent = NULL;

static int lws_addheader_mux_opcode(unsigned char *pb, int len)
{
	unsigned char *start = pb;

	*pb++ = LWS_WS_OPCODE_07__NOSPEC__MUX | 0x80;
	if (len < 126)
		*pb++ = len;
	else {
		if (len > 65535) {
			*pb++ = 127;
			*pb++ = 0;
			*pb++ = 0;
			*pb++ = 0;
			*pb++ = 0;
			*pb++ = (len ) >> 24;
			*pb++ = (len) >> 16;
			*pb++ = (len) >> 8;
			*pb++ = (len) >> 0;
		} else {
			*pb++ = 126;
			*pb++ = (len) >> 8;
			*pb++ = (len) >> 0;
		}
	}

	return pb - start;
}

static int lws_mux_subcommand_header(int cmd, int channel, unsigned char *pb, int len)
{
	unsigned char *start = pb;

	*pb++ = ((channel >> 8) << 3) | cmd;
	*pb++ = channel;

	if (len <= 253)
		*pb++ = len;
	else {
		*pb++ = 254;
		*pb++ = len >> 8;
		*pb++ = len;
	}

	return pb - start;
}

static int lws_ext_x_google_mux__send_addchannel(
	struct libwebsocket_context *context,
	struct libwebsocket *wsi,
	struct lws_ext_x_google_mux_conn *parent_conn,
	struct libwebsocket *wsi_child,
	int channel,
	const char *url
) {

	unsigned char send_buf[LWS_SEND_BUFFER_PRE_PADDING + 2048 +
						  LWS_SEND_BUFFER_POST_PADDING];
	unsigned char *pb = &send_buf[LWS_SEND_BUFFER_PRE_PADDING];
	char *p;
	char delta_headers[1536];
	int delta_headers_len;
	int subcommand_length;
	int n;

	wsi_child->ietf_spec_revision = wsi->ietf_spec_revision;

	p = libwebsockets_generate_client_handshake(context, wsi_child, delta_headers);
	delta_headers_len = p - delta_headers;

	subcommand_length = lws_mux_subcommand_header(LWS_EXT_XGM_OPC__ADDCHANNEL, channel, pb, delta_headers_len);

	pb += lws_addheader_mux_opcode(pb, subcommand_length + delta_headers_len);
	pb += lws_mux_subcommand_header(LWS_EXT_XGM_OPC__ADDCHANNEL, channel, pb, delta_headers_len);

//	n = sprintf((char *)pb, "%s\x0d\x0a", url);
//	pb += n;

	if (delta_headers_len)
		memcpy(pb, delta_headers, delta_headers_len);

	pb += delta_headers_len;

	muxdebug("add channel sends %ld\n",
				   pb - &send_buf[LWS_SEND_BUFFER_PRE_PADDING]);

	parent_conn->defeat_mux_opcode_wrapping = 1;

	/* send the request to the server */

	n = lws_issue_raw(wsi, &send_buf[LWS_SEND_BUFFER_PRE_PADDING],
				   pb - &send_buf[LWS_SEND_BUFFER_PRE_PADDING]);

	parent_conn->defeat_mux_opcode_wrapping = 0;

	return n;
}

/**
 * lws_extension_x_google_mux_parser():  Parse mux buffer headers coming in
 * 					 from a muxed connection into subchannel
 * 					 specific actions
 * @wsi:	muxed websocket instance
 * @conn:	x-google-mux private data bound to that @wsi
 * @c:		next character in muxed stream
 */

static int
lws_extension_x_google_mux_parser(struct libwebsocket_context *context,
			struct libwebsocket *wsi,
			struct libwebsocket_extension *this_ext,
			struct lws_ext_x_google_mux_conn *conn, unsigned char c)
{
	struct libwebsocket *wsi_child = NULL;
	struct libwebsocket_extension *ext;
	struct lws_ext_x_google_mux_conn *child_conn = NULL;
	int n;
	void *v;

//	fprintf(stderr, "XRX: %02X %d %d\n", c, conn->state, conn->length);

	/*
	 * [ <Channel ID b12.. b8> <Mux Opcode b2..b0> ]
	 * [ <Channel ID b7.. b0> ]
	 */
	
	switch (conn->state) {

	case LWS_EXT_XGM_STATE__MUX_BLOCK_1:
		muxdebug("LWS_EXT_XGM_STATE__MUX_BLOCK_1: opc=%d\n", c & 7);
		conn->block_subopcode = c & 7;
		conn->block_subchannel = (c << 5) & ~0xff;
		conn->state = LWS_EXT_XGM_STATE__MUX_BLOCK_2;
		break;

	case LWS_EXT_XGM_STATE__MUX_BLOCK_2:
		conn->block_subchannel |= c;
		muxdebug("LWS_EXT_XGM_STATE__MUX_BLOCK_2: subchannel=%d\n", conn->block_subchannel);

		ongoing_subchannel = ongoing_subchannel;

		/*
		 * convert the subchannel index to a child wsi
		 */

		/* act on the muxing opcode */
		
		switch (conn->block_subopcode) {
		case LWS_EXT_XGM_OPC__DATA:
			conn->state = LWS_EXT_XGM_STATE__DATA;
			break;
		case LWS_EXT_XGM_OPC__ADDCHANNEL:
			conn->state = LWS_EXT_XGM_STATE__ADDCHANNEL_LEN;
			switch (wsi->mode) {

			/* client: parse accepted headers returned by server */

			case LWS_CONNMODE_WS_CLIENT_WAITING_PROXY_REPLY:
			case LWS_CONNMODE_WS_CLIENT_ISSUE_HANDSHAKE:
			case LWS_CONNMODE_WS_CLIENT_WAITING_SERVER_REPLY:
			case LWS_CONNMODE_WS_CLIENT:
				wsi_child = conn->wsi_children[conn->block_subchannel];
				wsi_child->state = WSI_STATE_HTTP_HEADERS;
				wsi_child->parser_state = WSI_TOKEN_NAME_PART;
				break;
			default:
				wsi_child = libwebsocket_create_new_server_wsi(context);
				conn->wsi_children[conn->block_subchannel] = wsi_child;
				wsi_child->state = WSI_STATE_HTTP_HEADERS;
				wsi_child->parser_state = WSI_TOKEN_NAME_PART;
				wsi_child->extension_handles = wsi;
				muxdebug("MUX LWS_EXT_XGM_OPC__ADDCHANNEL... "
					"created child subchannel %d\n", conn->block_subchannel);
				break;
			}
			break;
		case LWS_EXT_XGM_OPC__DROPCHANNEL:
			conn->state = LWS_EXT_XGM_STATE__MUX_BLOCK_1;
			break;
		case LWS_EXT_XGM_OPC__FLOWCONTROL:
			conn->state = LWS_EXT_XGM_STATE__FLOWCONTROL_1;
			break;
		default:
			fprintf(stderr, "xgm: unknown subopcode\n");
			return -1;
		}
		break;

	case LWS_EXT_XGM_STATE__ADDCHANNEL_LEN:
		switch (c) {
		case 254:
			conn->state = LWS_EXT_XGM_STATE__ADDCHANNEL_LEN16_1;
			break;
		case 255:
			conn->state = LWS_EXT_XGM_STATE__ADDCHANNEL_LEN32_1;
			break;
		default:
			conn->length = c;
			conn->state = LWS_EXT_XGM_STATE__ADDCHANNEL_HEADERS;
			break;
		}
		break;

	case LWS_EXT_XGM_STATE__ADDCHANNEL_LEN16_1:
		conn->length = c << 8;
		conn->state = LWS_EXT_XGM_STATE__ADDCHANNEL_LEN16_2;
		break;

	case LWS_EXT_XGM_STATE__ADDCHANNEL_LEN16_2:
		conn->length |= c;
		conn->state = LWS_EXT_XGM_STATE__ADDCHANNEL_HEADERS;
		muxdebug("conn->length in mux block is %d\n", conn->length);
		break;

	case LWS_EXT_XGM_STATE__ADDCHANNEL_LEN32_1:
		conn->length = c << 24;
		conn->state = LWS_EXT_XGM_STATE__ADDCHANNEL_LEN32_2;
		break;

	case LWS_EXT_XGM_STATE__ADDCHANNEL_LEN32_2:
		conn->length |= c << 16;
		conn->state = LWS_EXT_XGM_STATE__ADDCHANNEL_LEN32_3;
		break;

	case LWS_EXT_XGM_STATE__ADDCHANNEL_LEN32_3:
		conn->length |= c << 8;
		conn->state = LWS_EXT_XGM_STATE__ADDCHANNEL_LEN32_4;
		break;

	case LWS_EXT_XGM_STATE__ADDCHANNEL_LEN32_4:
		conn->length |= c;
		conn->state = LWS_EXT_XGM_STATE__ADDCHANNEL_HEADERS;
		break;

	case LWS_EXT_XGM_STATE__ADDCHANNEL_HEADERS:

		switch (wsi->mode) {

		/* client: parse accepted headers returned by server */
			
		case LWS_CONNMODE_WS_CLIENT_WAITING_PROXY_REPLY:
		case LWS_CONNMODE_WS_CLIENT_ISSUE_HANDSHAKE:
		case LWS_CONNMODE_WS_CLIENT_WAITING_SERVER_REPLY:
		case LWS_CONNMODE_WS_CLIENT_WAITING_EXTENSION_CONNECT:
		case LWS_CONNMODE_WS_CLIENT:

			muxdebug("Client LWS_EXT_XGM_STATE__ADDCHANNEL_HEADERS in %c\n", c);
			wsi_child = conn->wsi_children[conn->block_subchannel];

			libwebsocket_parse(wsi_child, c);

			if (--conn->length)
				return 0;

			/* it's here we create the actual ext conn via callback */
			tag_with_parent = wsi;
			lws_client_interpret_server_handshake(context, wsi_child);
			tag_with_parent = NULL;

			//			if (wsi->parser_state != WSI_PARSING_COMPLETE)
//				break;

			/* client: we received all server's ADD ack */

			child_conn = lws_get_extension_user_matching_ext(wsi_child, this_ext);
			muxdebug("Received server's ADD Channel ACK for subchannel %d child_conn=%p!\n", conn->block_subchannel, (void *)child_conn);

			wsi_child->xor_mask = xor_no_mask;
			wsi_child->ietf_spec_revision = wsi->ietf_spec_revision;

			wsi_child->mode = LWS_CONNMODE_WS_CLIENT;
			wsi_child->state = WSI_STATE_ESTABLISHED;

			conn->state = LWS_EXT_XGM_STATE__MUX_BLOCK_1;
			child_conn->state = LWS_EXT_XGM_STATE__MUX_BLOCK_1;

			/* allocate the per-connection user memory (if any) */

			if (wsi_child->protocol->per_session_data_size) {
				wsi_child->user_space = malloc(
						wsi_child->protocol->per_session_data_size);
				if (wsi_child->user_space  == NULL) {
					fprintf(stderr, "Out of memory for "
								   "conn user space\n");
					goto bail2;
				}
			} else
				wsi_child->user_space = NULL;

			/* clear his proxy connection timeout */

			libwebsocket_set_timeout(wsi, NO_PENDING_TIMEOUT, 0);

			/* mark him as being alive */

			wsi_child->state = WSI_STATE_ESTABLISHED;
			wsi_child->mode = LWS_CONNMODE_WS_CLIENT;

			if (wsi_child->protocol)
				fprintf(stderr, "mux handshake OK for protocol %s\n",
					wsi_child->protocol->name);
			else
				fprintf(stderr, "mux child handshake ends up with no protocol!\n");

			/*
			 * inform all extensions, not just active ones since they
			 * already know
			 */

			ext = context->extensions;

			while (ext && ext->callback) {
				v = NULL;
				for (n = 0; n < wsi_child->count_active_extensions; n++)
					if (wsi_child->active_extensions[n] == ext) {
						v = wsi_child->active_extensions_user[n];
					}

				ext->callback(context, ext, wsi_child,
				      LWS_EXT_CALLBACK_ANY_WSI_ESTABLISHED, v, NULL, 0);
				ext++;
			}

			/* call him back to inform him he is up */

			wsi->protocol->callback(context, wsi_child,
					 LWS_CALLBACK_CLIENT_ESTABLISHED,
					 wsi_child->user_space,
					 NULL, 0);

			return 0;

bail2:
		exit(1);

		/* server: parse proposed changed headers from client */

		default:
			break;
		}

		/*
		 * SERVER
		 */

		wsi_child = conn->wsi_children[conn->block_subchannel];

		muxdebug("Server LWS_EXT_XGM_STATE__ADDCHANNEL_HEADERS in %d\n", conn->length);

		libwebsocket_read(context, wsi_child, &c, 1);

		if (--conn->length > 0)
			break;

		muxdebug("Server LWS_EXT_XGM_STATE__ADDCHANNEL_HEADERS done\n");

		/*
		 * server: header diffs are all seen, we must process
		 * the add action
		 */

		/* reply with ADDCHANNEL to ack it */

		wsi->xor_mask = xor_no_mask;
		child_conn = lws_get_extension_user_matching_ext(wsi_child, this_ext);
		child_conn->wsi_parent = wsi;
		child_conn->sticky_mux_used = 1;

		muxdebug("Setting child conn parent to %p\n", (void *)wsi);

//		lws_ext_x_google_mux__send_addchannel(context, wsi, conn, wsi_child,
//						    conn->block_subchannel, "url-parsing-not-done-yet");

		wsi_child->mode = LWS_CONNMODE_WS_SERVING;
		wsi_child->state = WSI_STATE_ESTABLISHED;
		wsi_child->lws_rx_parse_state = LWS_RXPS_NEW;
		wsi_child->rx_packet_length = 0;

		/* allocate the per-connection user memory (if any) */

		if (wsi_child->protocol->per_session_data_size) {
			wsi_child->user_space = malloc(
					  wsi_child->protocol->per_session_data_size);
			if (wsi_child->user_space  == NULL) {
				fprintf(stderr, "Out of memory for "
							   "conn user space\n");
				break;
			}
		} else
			wsi_child->user_space = NULL;


		conn->wsi_children[conn->block_subchannel] = wsi_child;
		if (conn->count_children <= conn->block_subchannel)
			conn->count_children = conn->block_subchannel + 1;


		/* notify user code that we're ready to roll */

		if (wsi_child->protocol->callback)
			wsi_child->protocol->callback(wsi_child->protocol->owning_server,
					wsi_child, LWS_CALLBACK_ESTABLISHED, wsi_child->user_space,
																	   NULL, 0);

		muxdebug("setting conn state to LWS_EXT_XGM_STATE__MUX_BLOCK_1\n");
		conn->state = LWS_EXT_XGM_STATE__MUX_BLOCK_1;
		break;

	case LWS_EXT_XGM_STATE__FLOWCONTROL_1:
		conn->length = c << 24;
		conn->state = LWS_EXT_XGM_STATE__FLOWCONTROL_2;
		break;

	case LWS_EXT_XGM_STATE__FLOWCONTROL_2:
		conn->length |= c << 16;
		conn->state = LWS_EXT_XGM_STATE__FLOWCONTROL_3;
		break;

	case LWS_EXT_XGM_STATE__FLOWCONTROL_3:
		conn->length |= c << 8;
		conn->state = LWS_EXT_XGM_STATE__FLOWCONTROL_4;
		break;

	case LWS_EXT_XGM_STATE__FLOWCONTROL_4:
		conn->length |= c;
		conn->state = LWS_EXT_XGM_STATE__MUX_BLOCK_1;
		break;

	case LWS_EXT_XGM_STATE__DATA:

//		fprintf(stderr, "LWS_EXT_XGM_STATE__DATA in\n");

		/*
		 * we have cooked websocket frame content following just like
		 * it went on the wire without mux, including masking and any
		 * other extensions (including this guy can himself be another
		 * level of channel mux, there's no restriction).
		 *
		 * We deal with it by just feeding it to the child wsi's rx
		 * state machine.  The only issue is, we need that state machine
		 * to tell us when it ate a full frame, so we watch its state
		 * afterwards
		 */
		if (conn->block_subchannel > conn->count_children) {
			fprintf(stderr, "Illegal subchannel\n");
			return -1;
		}

		wsi_child = conn->wsi_children[conn->block_subchannel];

		switch (wsi_child->mode) {

		/* client receives something */
			
		case LWS_CONNMODE_WS_CLIENT_WAITING_PROXY_REPLY:
		case LWS_CONNMODE_WS_CLIENT_ISSUE_HANDSHAKE:
		case LWS_CONNMODE_WS_CLIENT_WAITING_SERVER_REPLY:
		case LWS_CONNMODE_WS_CLIENT:
//			fprintf(stderr, "  client\n");
			if (libwebsocket_client_rx_sm(wsi_child, c) < 0) {
				libwebsocket_close_and_free_session(
					context,
					wsi_child,
					LWS_CLOSE_STATUS_GOINGAWAY);
			}

			return 0;

		/* server is receiving from client */

		default:
//			fprintf(stderr, "  server\n");
			if (libwebsocket_rx_sm(wsi_child, c) < 0) {
				fprintf(stderr, "probs\n");
				libwebsocket_close_and_free_session(
					context,
					wsi_child,
					LWS_CLOSE_STATUS_GOINGAWAY);
			}
			break;
		}
		break;
	}

	return 0;
}



int lws_extension_callback_x_google_mux(
	struct libwebsocket_context *context,
	struct libwebsocket_extension *ext,
	struct libwebsocket *wsi,
	enum libwebsocket_extension_callback_reasons reason,
					       void *user, void *in, size_t len)
{
	unsigned char send_buf[LWS_SEND_BUFFER_PRE_PADDING + 4096 +
						  LWS_SEND_BUFFER_POST_PADDING];
	struct lws_ext_x_google_mux_conn *conn =
				       (struct lws_ext_x_google_mux_conn *)user;
	struct lws_ext_x_google_mux_conn *parent_conn;
	struct lws_ext_x_google_mux_conn *child_conn;
	int n;
	struct lws_tokens *eff_buf = (struct lws_tokens *)in;
	unsigned char *p = NULL;
	struct lws_ext_x_google_mux_context *mux_ctx =
						  ext->per_context_private_data;
	struct libwebsocket *wsi_parent;
	struct libwebsocket *wsi_temp;
	unsigned char *pin = (unsigned char *)in;
	unsigned char *basepin;
	int m;
	int done = 0;
	unsigned char *pb = &send_buf[LWS_SEND_BUFFER_PRE_PADDING];
	int subcommand_length;

	if (eff_buf)
		p = (unsigned char *)eff_buf->token;

	switch (reason) {

	/* these guys are once per context */

	case LWS_EXT_CALLBACK_SERVER_CONTEXT_CONSTRUCT:
	case LWS_EXT_CALLBACK_CLIENT_CONTEXT_CONSTRUCT:

		ext->per_context_private_data = malloc(
				  sizeof (struct lws_ext_x_google_mux_context));
		mux_ctx = (struct lws_ext_x_google_mux_context *)
						  ext->per_context_private_data;
		mux_ctx->active_conns = 0;
		break;

	case LWS_EXT_CALLBACK_SERVER_CONTEXT_DESTRUCT:
	case LWS_EXT_CALLBACK_CLIENT_CONTEXT_DESTRUCT:

		if (mux_ctx) {
			for (n = 0; n < mux_ctx->active_conns; n++)
				if (mux_ctx->wsi_muxconns[n]) {
					libwebsocket_close_and_free_session(
						context,
						mux_ctx->wsi_muxconns[n],
						LWS_CLOSE_STATUS_GOINGAWAY);
					mux_ctx->wsi_muxconns[n] = NULL;
				}

			free(mux_ctx);
		}
		break;

	/*
	 * channel management
	 */

	case LWS_EXT_CALLBACK_CAN_PROXY_CLIENT_CONNECTION:

		muxdebug("LWS_EXT_CALLBACK_CAN_PROXY_CLIENT_CONNECTION %s:%u\n", (char *)in, (unsigned int)len);

		/*
		 * Does a physcial connection to the same server:port already
		 * exist so we can piggyback on it?
		 */

		for (n = 0; n < mux_ctx->active_conns && !done; n++) {

			wsi_parent = mux_ctx->wsi_muxconns[n];
			if (!wsi_parent)
				continue;

			muxdebug("  %s / %s\n", wsi_parent->c_address, (char *)in);
			if (strcmp(wsi_parent->c_address, in))
				continue;
			muxdebug("  %u / %u\n", wsi_parent->c_port, (unsigned int)len);

			if (wsi_parent->c_port != (unsigned int)len)
				continue;

			/*
			 * does this potential parent already have an
			 * x-google-mux conn associated with him?
			 */

			parent_conn = NULL;
			for (m = 0; m < wsi_parent->count_active_extensions; m++)
				if (ext == wsi_parent->active_extensions[m])
					parent_conn = (struct lws_ext_x_google_mux_conn *)
						wsi_parent->active_extensions_user[m];

			if (parent_conn == NULL) {

				/*
				 * he doesn't -- see if that's just because it
				 * is early in his connection sequence or if we
				 * should give up on him
				 */

				switch (wsi_parent->mode) {
				case LWS_CONNMODE_WS_SERVING:
				case LWS_CONNMODE_WS_CLIENT:
					continue;
				default:
					break;
				}

				/*
				 * our putative parent is still connecting
				 * himself, we have to become a candidate child
				 * and find out our final fate when the parent
				 * completes connection
				 */

				 wsi->candidate_children_list = wsi_parent->candidate_children_list;
				 wsi_parent->candidate_children_list = wsi;
				 wsi->mode = LWS_CONNMODE_WS_CLIENT_PENDING_CANDIDATE_CHILD;

				 done = 1;
				 continue;
			}

			if (parent_conn->count_children >=
					sizeof(parent_conn->wsi_children) /
					   sizeof(parent_conn->wsi_children[0]))
				continue;
			/*
			 * this established connection will do, bind them
			 * from now on child will only operate through parent
			 * connection
			 */

			wsi->candidate_children_list = wsi_parent->candidate_children_list;
			wsi_parent->candidate_children_list = wsi;
			wsi->mode = LWS_CONNMODE_WS_CLIENT_PENDING_CANDIDATE_CHILD;

			fprintf(stderr, "attaching to existing mux\n");

			conn = parent_conn;
			wsi = wsi_parent;

			goto handle_additions;

		}

		/*
		 * either way, note the existence of this connection in case
		 * he will become a possible mux parent later
		 */

		mux_ctx->wsi_muxconns[mux_ctx->active_conns++] = wsi;
		if (done)
			return 1;

		fprintf(stderr, "x-google-mux: unable to mux connection\n");

		break;

	/* these guys are once per connection */

	case LWS_EXT_CALLBACK_CLIENT_CONSTRUCT:
		muxdebug("LWS_EXT_CALLBACK_CLIENT_CONSTRUCT: setting parent = %p\n", (void *)tag_with_parent);
		conn->state = LWS_EXT_XGM_STATE__MUX_BLOCK_1;
		conn->wsi_parent = tag_with_parent;
		break;

	case LWS_EXT_CALLBACK_CONSTRUCT:
		muxdebug("LWS_EXT_CALLBACK_CONSTRUCT\n");
		conn->state = LWS_EXT_XGM_STATE__MUX_BLOCK_1;
		break;

	case LWS_EXT_CALLBACK_DESTROY:
		muxdebug("LWS_EXT_CALLBACK_DESTROY\n");

		/*
		 * remove us from parent if noted in parent
		 */

		if (conn->wsi_parent) {

			parent_conn = lws_get_extension_user_matching_ext(conn->wsi_parent, ext);
			if (parent_conn == 0) {
				fprintf(stderr, "failed to get parent conn\n");
				break;
			}
			for (n = 0; n < parent_conn->count_children; n++)
				if (parent_conn->wsi_children[n] == wsi) {
					parent_conn->count_children--;
					while (n < parent_conn->count_children) {
						parent_conn->wsi_children[n] = parent_conn->wsi_children[n + 1];
						n++;
					}
				}
		}

		break;

	case LWS_EXT_CALLBACK_DESTROY_ANY_WSI_CLOSING:
		muxdebug("LWS_EXT_CALLBACK_DESTROY_ANY_WSI_CLOSING\n");

		for (n = 0; n < mux_ctx->active_conns; n++)
			if (mux_ctx->wsi_muxconns[n] == wsi) {
				while (n++ < mux_ctx->active_conns)
					mux_ctx->wsi_muxconns[n - 1] =
						       mux_ctx->wsi_muxconns[n];
				mux_ctx->active_conns--;
				return 0;
			}

		/*
		 * liberate any candidate children otherwise imprisoned
		 */

		wsi_parent = wsi->candidate_children_list;
		while (wsi_parent) {
			wsi_temp = wsi_parent->candidate_children_list;
			/* let them each connect privately then */
			__libwebsocket_client_connect_2(context, wsi_parent);
			wsi_parent = wsi_temp;
		}

		break;

	case LWS_EXT_CALLBACK_ANY_WSI_ESTABLISHED:
		muxdebug("LWS_EXT_CALLBACK_ANY_WSI_ESTABLISHED\n");

handle_additions:
		/*
		 * did this putative parent get x-google-mux authorized in the
		 * end?
		 */

		if (!conn) {

			muxdebug("  Putative parent didn't get mux extension, let them go it alone\n");

			/*
			 * no, we can't be a parent for mux children.  Let
			 * them all go it alone
			 */

			wsi_parent = wsi->candidate_children_list;
			while (wsi_parent) {
				wsi_temp = wsi_parent->candidate_children_list;
				/* let them each connect privately then */
				__libwebsocket_client_connect_2(context, wsi_parent);
				wsi_parent = wsi_temp;
			}

			return 1;
		}

		/*
		 * we did get mux extension authorized by server, in that case
		 * if we have any candidate children let's try to attach them
		 * as mux subchannel real children
		 */
		
		wsi_parent = wsi->candidate_children_list;
		while (wsi_parent) {

			muxdebug("  using mux addchannel action for candidate child\n");
			
			wsi_temp = wsi_parent->candidate_children_list;
			/* let them each connect privately then */
			lws_ext_x_google_mux__send_addchannel(context, wsi,
					conn, wsi_parent,
					     conn->count_children, wsi->c_path);

			conn->sticky_mux_used = 1;

			conn->wsi_children[conn->count_children++] = wsi_parent;
			muxdebug("Setting CHILD LIST entry %d to %p\n",
				  conn->count_children - 1, (void *)wsi_parent);
			wsi_parent = wsi_temp;
		}
		wsi->candidate_children_list = NULL;
		return 1;

	/*
	 * whenever we receive something on a muxed link
	 */

	case LWS_EXT_CALLBACK_EXTENDED_PAYLOAD_RX:

		muxdebug("LWS_EXT_CALLBACK_EXTENDED_PAYLOAD_RX\n");

		if (wsi->opcode != LWS_WS_OPCODE_07__NOSPEC__MUX)
			return 0; /* unhandled */

		conn->state = LWS_EXT_XGM_STATE__MUX_BLOCK_1;

		n = eff_buf->token_len;
		while (n--)
			lws_extension_x_google_mux_parser(context, wsi, ext, conn, *p++);

		return 1; /* handled */

	/*
	 * when something might need sending on our transport
	 */

	case LWS_EXT_CALLBACK_PACKET_TX_DO_SEND:

		muxdebug("LWS_EXT_CALLBACK_PACKET_TX_DO_SEND: %p\n", (void *)conn->wsi_parent);

		pin = *((unsigned char **)in);
		basepin = pin;

		/*
		 * he's not a child connection of a mux
		 */

		if (!conn->wsi_parent) {
//			fprintf(stderr, "conn %p has no parent\n", (void *)conn);
			return 0;
		}

		/*
		 * get parent / transport mux context
		 */

		parent_conn = lws_get_extension_user_matching_ext(conn->wsi_parent, ext);
		if (parent_conn == 0) {
			fprintf(stderr, "failed to get parent conn\n");
			return 0;
		}

		/*
		 * mux transport is in singular mode, let the caller send it
		 * no more muxified than it already is
		 */

		if (!parent_conn->sticky_mux_used) {
//			fprintf(stderr, "parent in singular mode\n");
			return 0;
		}

		if (!conn->defeat_mux_opcode_wrapping) {

			/*
			 * otherwise we need to take care of the sending action using
			 * mux protocol.  Prepend the channel + opcode
			 */

			pin -= lws_addheader_mux_opcode(send_buf, len + 2) + 2;
			basepin = pin;
			pin += lws_addheader_mux_opcode(pin, len + 2);

			*pin++ = (conn->subchannel >> 8) | LWS_EXT_XGM_OPC__DATA;
			*pin++ = conn->subchannel;

		}

		/*
		 * recurse to allow nesting
		 */

		lws_issue_raw(conn->wsi_parent, basepin, (pin - basepin) + len);

		return 1; /* handled */

	case LWS_EXT_CALLBACK_1HZ:
		/*
		 * if we have children, service their timeouts using the same
		 * handler as toplevel guys to allow recursion
		 */
		for (n = 0; n < conn->count_children; n++)
			libwebsocket_service_timeout_check(context,
						    conn->wsi_children[n], len);
		break;

	case LWS_EXT_CALLBACK_REQUEST_ON_WRITEABLE:
		/*
		 * if a mux child is asking for callback on writable, we have
		 * to pass it up to his parent
		 */

		muxdebug("LWS_EXT_CALLBACK_REQUEST_ON_WRITEABLE %s\n", wsi->protocol->name);

		if (conn->wsi_parent == NULL) {
			muxdebug("  no parent\n");
			break;
		}

		if (!conn->awaiting_POLLOUT) {

			muxdebug("  !conn->awaiting_POLLOUT\n");

			conn->awaiting_POLLOUT = 1;
			parent_conn = NULL;
			for (m = 0; m < conn->wsi_parent->count_active_extensions; m++)
				if (ext == conn->wsi_parent->active_extensions[m])
					parent_conn = (struct lws_ext_x_google_mux_conn *)
						conn->wsi_parent->active_extensions_user[m];

			if (parent_conn != NULL) {
				parent_conn->count_children_needing_POLLOUT++;
				muxdebug("  count_children_needing_POLLOUT bumped\n");
			} else
				fprintf(stderr, "unable to identify parent conn\n");
		}
		muxdebug("  requesting on parent %p\n", (void *)conn->wsi_parent);
		libwebsocket_callback_on_writable(context, conn->wsi_parent);

		return 1;

	case LWS_EXT_CALLBACK_HANDSHAKE_REPLY_TX:

		fprintf(stderr, "LWS_EXT_CALLBACK_HANDSHAKE_REPLY_TX %p\n", (void *)wsi->extension_handles);

		/* send raw if we're not a child */

		if (!wsi->extension_handles)
			return 0;

		subcommand_length = lws_mux_subcommand_header(LWS_EXT_XGM_OPC__ADDCHANNEL, ongoing_subchannel, pb, len);

		pb += lws_addheader_mux_opcode(pb, subcommand_length + len);
		pb += lws_mux_subcommand_header(LWS_EXT_XGM_OPC__ADDCHANNEL, ongoing_subchannel, pb, len);
		memcpy(pb, in, len);
		pb += len;

		lws_issue_raw(wsi->extension_handles, &send_buf[LWS_SEND_BUFFER_PRE_PADDING],
					   pb - &send_buf[LWS_SEND_BUFFER_PRE_PADDING]);


		return 1; /* handled */

	case LWS_EXT_CALLBACK_IS_WRITEABLE:
		/*
		 * we are writable, inform children if any care
		 */
		muxdebug("LWS_EXT_CALLBACK_IS_WRITEABLE: %s\n", wsi->protocol->name);

		if (!conn->count_children_needing_POLLOUT) {
			muxdebug("  no children need POLLOUT\n");
			return 0;
		}

		for (n = 0; n < conn->count_children; n++) {

			child_conn = NULL;
			for (m = 0; m < conn->wsi_children[n]->count_active_extensions; m++)
				if (ext == conn->wsi_children[n]->active_extensions[m])
					child_conn = (struct lws_ext_x_google_mux_conn *)
							conn->wsi_children[n]->active_extensions_user[m];

			if (!child_conn) {
				fprintf(stderr, "unable to identify child conn\n");
				continue;
			}

			if (!child_conn->awaiting_POLLOUT)
				continue;

			child_conn->awaiting_POLLOUT = 0;
			conn->count_children_needing_POLLOUT--;
			lws_handle_POLLOUT_event(context, conn->wsi_children[n], NULL);
			if (!conn->count_children_needing_POLLOUT)
				return 2; /* all handled */
			else
				return 1; /* handled but need more */
		}
		break;

	default:
		break;
	}

	return 0;
}
