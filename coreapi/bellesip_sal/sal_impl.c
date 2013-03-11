/*
linphone
Copyright (C) 2012  Belledonne Communications, Grenoble, France

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/


#include "sal_impl.h"

void _belle_sip_log(belle_sip_log_level lev, const char *fmt, va_list args) {
	int ortp_level;
	switch(lev) {
		case BELLE_SIP_LOG_FATAL:
			ortp_level=ORTP_FATAL;
		break;
		case BELLE_SIP_LOG_ERROR:
			ortp_level=ORTP_ERROR;
		break;
		case BELLE_SIP_LOG_WARNING:
			ortp_level=ORTP_WARNING;
		break;
		case BELLE_SIP_LOG_MESSAGE:
			ortp_level=ORTP_MESSAGE;
		break;
		case BELLE_SIP_LOG_DEBUG:
		default:
			ortp_level=ORTP_DEBUG;
			break;
	}
	if (ortp_log_level_enabled(ortp_level)){
		ortp_logv(ortp_level,fmt,args);
	}
}

void sal_enable_logs(){
	belle_sip_set_log_level(BELLE_SIP_LOG_MESSAGE);
}
void sal_disable_logs() {
	belle_sip_set_log_level(BELLE_SIP_LOG_ERROR);
}
static void sal_add_pending_auth(Sal *sal, SalOp *op){
	sal->pending_auths=ms_list_append(sal->pending_auths,op);
}

 void sal_remove_pending_auth(Sal *sal, SalOp *op){
	sal->pending_auths=ms_list_remove(sal->pending_auths,op);
}

void sal_process_authentication(SalOp *op, belle_sip_response_t *response) {
	belle_sip_request_t* request;
	bool_t is_within_dialog=FALSE;
	belle_sip_list_t* auth_list=NULL;
	belle_sip_auth_event_t* auth_event;
	if (op->dialog && belle_sip_dialog_get_state(op->dialog)==BELLE_SIP_DIALOG_CONFIRMED) {
		request = belle_sip_dialog_create_request_from(op->dialog,(const belle_sip_request_t *)op->request);
		is_within_dialog=TRUE;
	} else {
		request=op->request;
		belle_sip_message_remove_header(BELLE_SIP_MESSAGE(request),BELLE_SIP_AUTHORIZATION);
		belle_sip_message_remove_header(BELLE_SIP_MESSAGE(request),BELLE_SIP_PROXY_AUTHORIZATION);

	}
	if (belle_sip_provider_add_authorization(op->base.root->prov,request,response,&auth_list)) {
		if (is_within_dialog) {
			sal_op_send_request(op,request);
		} else {
			sal_op_resend_request(op,request);
		}
	}else {
		ms_message("No auth info found for [%s]",sal_op_get_from(op));
		if (is_within_dialog) {
			belle_sip_object_unref(request);
		}
		if (op->auth_info) sal_auth_info_delete(op->auth_info);
		auth_event=(belle_sip_auth_event_t*)(auth_list->data);
		op->auth_info=sal_auth_info_new();
		op->auth_info->realm = ms_strdup(belle_sip_auth_event_get_realm(auth_event)) ;
		op->auth_info->username = ms_strdup(belle_sip_auth_event_get_username(auth_event)) ;
		belle_sip_list_free_with_data(auth_list,(void (*)(void*))belle_sip_auth_event_destroy);
		sal_add_pending_auth(op->base.root,op);
	}

}
static void process_dialog_terminated(void *sal, const belle_sip_dialog_terminated_event_t *event){
	belle_sip_dialog_t* dialog =  belle_sip_dialog_terminated_get_dialog(event);
	SalOp* op = belle_sip_dialog_get_application_data(dialog);
	if (op && op->callbacks.process_dialog_terminated) {
		op->callbacks.process_dialog_terminated(op,event);
	} else {
		ms_error("sal process_dialog_terminated no op found for this dialog [%p], ignoring",dialog);
	}
}
static void process_io_error(void *user_ctx, const belle_sip_io_error_event_t *event){
	belle_sip_client_transaction_t*client_transaction;
	SalOp* op;
	if (belle_sip_object_is_instance_of(BELLE_SIP_OBJECT(belle_sip_io_error_event_get_source(event)),BELLE_SIP_TYPE_ID(belle_sip_client_transaction_t))) {
		client_transaction=BELLE_SIP_CLIENT_TRANSACTION(belle_sip_io_error_event_get_source(event));
		op = (SalOp*)belle_sip_transaction_get_application_data(BELLE_SIP_TRANSACTION(client_transaction));
		if (op->callbacks.process_io_error) {
				op->callbacks.process_io_error(op,event);
		}
	} else {
		ms_error("sal process_io_error not implemented yet for non transaction");
	}
}
static void process_request_event(void *sal, const belle_sip_request_event_t *event) {
	SalOp* op=NULL;
	belle_sip_request_t* req = belle_sip_request_event_get_request(event);
	belle_sip_dialog_t* dialog=belle_sip_request_event_get_dialog(event);
	belle_sip_header_address_t* origin_address;
	belle_sip_header_address_t* address;
	belle_sip_header_from_t* from_header;
	belle_sip_header_to_t* to;
	belle_sip_response_t* resp;

	from_header=belle_sip_message_get_header_by_type(BELLE_SIP_MESSAGE(req),belle_sip_header_from_t);

	if (dialog) {
		op=(SalOp*)belle_sip_dialog_get_application_data(dialog);
	} else if (strcmp("INVITE",belle_sip_request_get_method(req))==0) {
		op=sal_op_new((Sal*)sal);
		op->dir=SalOpDirIncoming;
		sal_op_call_fill_cbs(op);
	} else if (strcmp("SUBSCRIBE",belle_sip_request_get_method(req))==0) {
		op=sal_op_new((Sal*)sal);
		op->dir=SalOpDirIncoming;
		sal_op_presence_fill_cbs(op);
	} else if (strcmp("MESSAGE",belle_sip_request_get_method(req))==0) {
		op=sal_op_new((Sal*)sal);
		op->dir=SalOpDirIncoming;
		sal_op_message_fill_cbs(op);

	} else if (strcmp("OPTIONS",belle_sip_request_get_method(req))==0) {
		resp=belle_sip_response_create_from_request(req,200);
		belle_sip_provider_send_response(((Sal*)sal)->prov,resp);
		return;
	}else {
		ms_error("sal process_request_event not implemented yet for method [%s]",belle_sip_request_get_method(req));
		resp=belle_sip_response_create_from_request(req,501);
		belle_sip_provider_send_response(((Sal*)sal)->prov,resp);
		return;
	}

	if (!op->base.from_address)  {
		address=belle_sip_header_address_create(belle_sip_header_address_get_displayname(BELLE_SIP_HEADER_ADDRESS(from_header))
														,belle_sip_header_address_get_uri(BELLE_SIP_HEADER_ADDRESS(from_header)));
		sal_op_set_from_address(op,(SalAddress*)address);
		belle_sip_object_unref(address);
	}


	if (!op->base.to_address) {
		to=belle_sip_message_get_header_by_type(BELLE_SIP_MESSAGE(req),belle_sip_header_to_t);
		address=belle_sip_header_address_create(belle_sip_header_address_get_displayname(BELLE_SIP_HEADER_ADDRESS(to))
												,belle_sip_header_address_get_uri(BELLE_SIP_HEADER_ADDRESS(to)));
		sal_op_set_to_address(op,(SalAddress*)address);
		belle_sip_object_unref(address);
	}

	if (!op->base.origin) {
		/*set origin uri*/
		origin_address=belle_sip_header_address_create(NULL,belle_sip_request_extract_origin(req));
		__sal_op_set_network_origin_address(op,(SalAddress*)origin_address);
		belle_sip_object_unref(origin_address);
	}
	if (!op->base.remote_ua) {
		sal_op_set_remote_ua(op,BELLE_SIP_MESSAGE(req));
	}

	if (!op->base.call_id) {
		op->base.call_id=ms_strdup(belle_sip_header_call_id_get_call_id(BELLE_SIP_HEADER_CALL_ID(belle_sip_message_get_header_by_type(BELLE_SIP_MESSAGE(req), belle_sip_header_call_id_t))));
	}
	if (op->callbacks.process_request_event) {
		op->callbacks.process_request_event(op,event);
	} else {
		ms_error("sal process_request_event not implemented yet");
	}

}

static void process_response_event(void *user_ctx, const belle_sip_response_event_t *event){
	belle_sip_client_transaction_t* client_transaction = belle_sip_response_event_get_client_transaction(event);
	belle_sip_response_t* response = belle_sip_response_event_get_response(event);
	int response_code = belle_sip_response_get_status_code(response);
	if (!client_transaction) {
		ms_warning("Discarding state less response [%i]",response_code);
		return;
	} else {
		SalOp* op = (SalOp*)belle_sip_transaction_get_application_data(BELLE_SIP_TRANSACTION(client_transaction));
		belle_sip_request_t* request=belle_sip_transaction_get_request(BELLE_SIP_TRANSACTION(client_transaction));
		belle_sip_header_contact_t* original_contact;
		belle_sip_header_address_t* contact_address=NULL;
		belle_sip_header_via_t* via_header;
		belle_sip_uri_t* contact_uri;
		unsigned int contact_port;
		const char* received;
		int rport;
		bool_t contact_updated=FALSE;
		char* new_contact;
		belle_sip_request_t* old_request=NULL;
		belle_sip_response_t* old_response=NULL;


		if (op->state == SalOpStateTerminated) {
			belle_sip_message("Op is terminated, nothing to do with this [%i]",response_code);
			return;
		}
		if (!op->base.remote_ua) {
			sal_op_set_remote_ua(op,BELLE_SIP_MESSAGE(response));
		}
		if (!op->base.call_id) {
			op->base.call_id=ms_strdup(belle_sip_header_call_id_get_call_id(BELLE_SIP_HEADER_CALL_ID(belle_sip_message_get_header_by_type(BELLE_SIP_MESSAGE(response), belle_sip_header_call_id_t))));
		}
		if (op->callbacks.process_response_event) {

			if (op->base.root->nat_helper_enabled) {
				/*Fix contact if needed*/
				via_header= (belle_sip_header_via_t*)belle_sip_message_get_header(BELLE_SIP_MESSAGE(response),BELLE_SIP_VIA);
				received = belle_sip_header_via_get_received(via_header);
				rport = belle_sip_header_via_get_rport(via_header);
				if ((original_contact=belle_sip_message_get_header_by_type(request,belle_sip_header_contact_t))) {
					/*update contact with sent values in any cases*/
					contact_address=belle_sip_header_address_create(NULL,belle_sip_header_address_get_uri(BELLE_SIP_HEADER_ADDRESS(original_contact)));
					sal_op_set_contact_address(op,(const SalAddress *)contact_address);
					belle_sip_object_unref(contact_address);
				}
				if (sal_op_get_contact(op)){
					if (received!=NULL || rport>0) {

						contact_address = BELLE_SIP_HEADER_ADDRESS(sal_address_clone(sal_op_get_contact_address(op)));
						contact_uri=belle_sip_header_address_get_uri(BELLE_SIP_HEADER_ADDRESS(contact_address));
						if (received && strcmp(received,belle_sip_uri_get_host(contact_uri))!=0) {
							/*need to update host*/
							belle_sip_uri_set_host(contact_uri,received);
							contact_updated=TRUE;
						}
						contact_port =  belle_sip_uri_get_port(contact_uri);
						if (rport>0 && rport!=contact_port && (contact_port+rport)!=5060) {
							/*need to update port*/
							belle_sip_uri_set_port(contact_uri,rport);
							contact_updated=TRUE;
						}

						/*try to fix transport if needed (very unlikely)*/
						if (strcasecmp(belle_sip_header_via_get_transport(via_header),"UDP")!=0) {
							if (!belle_sip_uri_get_transport_param(contact_uri)
									||strcasecmp(belle_sip_uri_get_transport_param(contact_uri),belle_sip_header_via_get_transport(via_header))!=0) {
								belle_sip_uri_set_transport_param(contact_uri,belle_sip_header_via_get_transport_lowercase(via_header));
								contact_updated=TRUE;
							}
						} else {
							if (belle_sip_uri_get_transport_param(contact_uri)) {
								contact_updated=TRUE;
								belle_sip_uri_set_transport_param(contact_uri,NULL);
							}
						}
						if (contact_updated) {
							char* old_contact=belle_sip_object_to_string(BELLE_SIP_OBJECT(sal_op_get_contact_address(op)));
							new_contact=belle_sip_object_to_string(BELLE_SIP_OBJECT(contact_address));
							ms_message("Updating contact from [%s] to [%s] for [%p]",old_contact,new_contact,op);
							sal_op_set_contact_address(op,(const SalAddress *)contact_address);
							belle_sip_free(new_contact);
							belle_sip_free(old_contact);
						}
						if (contact_address)belle_sip_object_unref(contact_address);
					}
				}
			}
			/*update request/response
			 * maybe only the transaction should be kept*/
			old_request=op->request;
			op->request=belle_sip_transaction_get_request(BELLE_SIP_TRANSACTION(client_transaction));
			belle_sip_object_ref(op->request);
			if (old_request) belle_sip_object_unref(old_request);

			old_response=op->response;
			op->response=response; /*kept for use at authorization time*/
			belle_sip_object_ref(op->response);
			if (old_response) belle_sip_object_unref(old_response);

			/*handle authozation*/
			switch (response_code) {
			case 200: {
				sal_remove_pending_auth(op->base.root,op);/*just in case*/
				break;
			}
			case 401:
			case 407:{
				/*belle_sip_transaction_set_application_data(BELLE_SIP_TRANSACTION(client_transaction),NULL);*//*remove op from trans*/
				if (op->state == SalOpStateTerminating && strcmp("BYE",belle_sip_request_get_method(request))!=0) {
					/*only bye are completed*/
					belle_sip_message("Op is in state terminating, nothing else to do ");
					return;
				} else {
					sal_process_authentication(op,response);
					return;
				}
			}
			}

			op->callbacks.process_response_event(op,event);

		} else {
			ms_error("Unhandled event response [%p]",event);
		}
	}

}
static void process_timeout(void *user_ctx, const belle_sip_timeout_event_t *event) {
	belle_sip_client_transaction_t* client_transaction = belle_sip_timeout_event_get_client_transaction(event);
	SalOp* op = (SalOp*)belle_sip_transaction_get_application_data(BELLE_SIP_TRANSACTION(client_transaction));
	if (op && op->callbacks.process_timeout) {
		op->callbacks.process_timeout(op,event);
	} else {
		ms_error("Unhandled event timeout [%p]",event);
	}
}
static void process_transaction_terminated(void *user_ctx, const belle_sip_transaction_terminated_event_t *event) {
	belle_sip_client_transaction_t* client_transaction = belle_sip_transaction_terminated_event_get_client_transaction(event);
	belle_sip_server_transaction_t* server_transaction = belle_sip_transaction_terminated_event_get_server_transaction(event);
	belle_sip_transaction_t* trans;
	SalOp* op;

	if(client_transaction)
		trans=BELLE_SIP_TRANSACTION(client_transaction);
	 else
		 trans=BELLE_SIP_TRANSACTION(server_transaction);

	op = (SalOp*)belle_sip_transaction_get_application_data(trans);
	if (op && op->callbacks.process_transaction_terminated) {
		op->callbacks.process_transaction_terminated(op,event);
	} else {
		ms_error("Unhandled transaction terminated [%p]",trans);
	}
	if (op) sal_op_unref(op); /*no longuer need to ref op*/
}
static void process_auth_requested(void *sal, belle_sip_auth_event_t *auth_event) {
	SalAuthInfo auth_info;
	memset(&auth_info,0,sizeof(SalAuthInfo));
	auth_info.username=(char*)belle_sip_auth_event_get_username(auth_event);
	auth_info.realm=(char*)belle_sip_auth_event_get_realm(auth_event);
	((Sal*)sal)->callbacks.auth_requested(sal,&auth_info);
	belle_sip_auth_event_set_passwd(auth_event,(const char*)auth_info.password);
	belle_sip_auth_event_set_ha1(auth_event,(const char*)auth_info.ha1);
	belle_sip_auth_event_set_userid(auth_event,(const char*)auth_info.userid);
	return;
}
Sal * sal_init(){
	char stack_string[64];
	belle_sip_listener_t* listener;
	Sal * sal=ms_new0(Sal,1);
	sal->nat_helper_enabled=TRUE;
	snprintf(stack_string,sizeof(stack_string)-1,"(belle-sip/%s)",belle_sip_version_to_string());
	sal->user_agent=belle_sip_header_user_agent_new();
#if defined(PACKAGE_NAME) && defined(LINPHONE_VERSION)
	belle_sip_header_user_agent_add_product(sal->user_agent, PACKAGE_NAME "/" LINPHONE_VERSION);
#endif

	belle_sip_header_user_agent_add_product(sal->user_agent,stack_string);
	belle_sip_object_ref(sal->user_agent);
	belle_sip_set_log_handler(_belle_sip_log);
	sal->stack = belle_sip_stack_new(NULL);
	sal->prov = belle_sip_stack_create_provider(sal->stack,NULL);
	sal->listener_callbacks.process_dialog_terminated=process_dialog_terminated;
	sal->listener_callbacks.process_io_error=process_io_error;
	sal->listener_callbacks.process_request_event=process_request_event;
	sal->listener_callbacks.process_response_event=process_response_event;
	sal->listener_callbacks.process_timeout=process_timeout;
	sal->listener_callbacks.process_transaction_terminated=process_transaction_terminated;
	sal->listener_callbacks.process_auth_requested=process_auth_requested;
	belle_sip_provider_add_sip_listener(sal->prov,listener=belle_sip_listener_create_from_callbacks(&sal->listener_callbacks,sal));
	/* belle_sip_callbacks_t is unowned, why ?belle_sip_object_unref(listener);*/
	return sal;
}
void sal_set_user_pointer(Sal *sal, void *user_data){
	sal->up=user_data;
}

void *sal_get_user_pointer(const Sal *sal){
	return sal->up;
}

static void unimplemented_stub(){
	ms_warning("Unimplemented SAL callback");
}

void sal_set_callbacks(Sal *ctx, const SalCallbacks *cbs){
	memcpy(&ctx->callbacks,cbs,sizeof(*cbs));
	if (ctx->callbacks.call_received==NULL)
		ctx->callbacks.call_received=(SalOnCallReceived)unimplemented_stub;
	if (ctx->callbacks.call_ringing==NULL)
		ctx->callbacks.call_ringing=(SalOnCallRinging)unimplemented_stub;
	if (ctx->callbacks.call_accepted==NULL)
		ctx->callbacks.call_accepted=(SalOnCallAccepted)unimplemented_stub;
	if (ctx->callbacks.call_failure==NULL)
		ctx->callbacks.call_failure=(SalOnCallFailure)unimplemented_stub;
	if (ctx->callbacks.call_terminated==NULL)
		ctx->callbacks.call_terminated=(SalOnCallTerminated)unimplemented_stub;
	if (ctx->callbacks.call_released==NULL)
		ctx->callbacks.call_released=(SalOnCallReleased)unimplemented_stub;
	if (ctx->callbacks.call_updating==NULL)
		ctx->callbacks.call_updating=(SalOnCallUpdating)unimplemented_stub;
	if (ctx->callbacks.auth_requested_legacy==NULL)
		ctx->callbacks.auth_requested_legacy=(SalOnAuthRequestedLegacy)unimplemented_stub;
	if (ctx->callbacks.auth_success==NULL)
		ctx->callbacks.auth_success=(SalOnAuthSuccess)unimplemented_stub;
	if (ctx->callbacks.register_success==NULL)
		ctx->callbacks.register_success=(SalOnRegisterSuccess)unimplemented_stub;
	if (ctx->callbacks.register_failure==NULL)
		ctx->callbacks.register_failure=(SalOnRegisterFailure)unimplemented_stub;
	if (ctx->callbacks.dtmf_received==NULL)
		ctx->callbacks.dtmf_received=(SalOnDtmfReceived)unimplemented_stub;
	if (ctx->callbacks.notify==NULL)
		ctx->callbacks.notify=(SalOnNotify)unimplemented_stub;
	if (ctx->callbacks.notify_presence==NULL)
		ctx->callbacks.notify_presence=(SalOnNotifyPresence)unimplemented_stub;
	if (ctx->callbacks.subscribe_received==NULL)
		ctx->callbacks.subscribe_received=(SalOnSubscribeReceived)unimplemented_stub;
	if (ctx->callbacks.text_received==NULL)
		ctx->callbacks.text_received=(SalOnTextReceived)unimplemented_stub;
	if (ctx->callbacks.ping_reply==NULL)
		ctx->callbacks.ping_reply=(SalOnPingReply)unimplemented_stub;
	if (ctx->callbacks.auth_requested==NULL)
		ctx->callbacks.auth_requested=(SalOnAuthRequested)unimplemented_stub;
}



void sal_uninit(Sal* sal){
	belle_sip_object_unref(sal->user_agent);
	belle_sip_object_unref(sal->prov);
	belle_sip_object_unref(sal->stack);
	ms_free(sal);
	return ;
};

int sal_add_listen_port(Sal *ctx, SalAddress* addr){
	int result;
	belle_sip_listening_point_t* lp = belle_sip_stack_create_listening_point(ctx->stack
																			,sal_address_get_domain(addr)
																			,sal_address_get_port_int(addr)
																			,sal_transport_to_string(sal_address_get_transport(addr)));
	if (lp) {
		belle_sip_listening_point_set_keep_alive(lp,ctx->keep_alive);
		result = belle_sip_provider_add_listening_point(ctx->prov,lp);
	} else {
		return -1;
	}
	return result;
}
int sal_listen_port(Sal *ctx, const char *addr, int port, SalTransport tr, int is_secure) {
	SalAddress* sal_addr = sal_address_new(NULL);
	int result;
	sal_address_set_domain(sal_addr,addr);
	sal_address_set_port_int(sal_addr,port);
	sal_address_set_transport(sal_addr,tr);
	result = sal_add_listen_port(ctx,sal_addr);
	sal_address_destroy(sal_addr);
	return result;
}
static void remove_listening_point(belle_sip_listening_point_t* lp,belle_sip_provider_t* prov) {
	belle_sip_provider_remove_listening_point(prov,lp);
}
int sal_unlisten_ports(Sal *ctx){
	const belle_sip_list_t * lps = belle_sip_provider_get_listening_points(ctx->prov);
	belle_sip_list_t * tmp_list = belle_sip_list_copy(lps);
	belle_sip_list_for_each2 (tmp_list,(void (*)(void*,void*))remove_listening_point,ctx->prov);
	belle_sip_list_free(tmp_list);

	ms_message("sal_unlisten_ports done");
	return 0;
}
ortp_socket_t sal_get_socket(Sal *ctx){
	ms_fatal("sal_get_socket not implemented yet");
	return -1;
}
void sal_set_user_agent(Sal *ctx, const char *user_agent){
	belle_sip_header_user_agent_set_products(ctx->user_agent,NULL);
	belle_sip_header_user_agent_add_product(ctx->user_agent,user_agent);
	return ;
}
/*keepalive period in ms*/
void sal_set_keepalive_period(Sal *ctx,unsigned int value){
	const belle_sip_list_t* iterator;
	belle_sip_listening_point_t* lp;
	ctx->keep_alive=value;
	for (iterator=belle_sip_provider_get_listening_points(ctx->prov);iterator!=NULL;iterator=iterator->next) {
		lp=(belle_sip_listening_point_t*)iterator->data;
		if (ctx->use_tcp_tls_keep_alive || strcasecmp(belle_sip_listening_point_get_transport(lp),"udp")==0) {
			belle_sip_listening_point_set_keep_alive(lp,ctx->keep_alive);
		}
	}
	return ;
}
/**
 * returns keepalive period in ms
 * 0 desactiaved
 * */
unsigned int sal_get_keepalive_period(Sal *ctx){
	return ctx->keep_alive;
}
void sal_use_session_timers(Sal *ctx, int expires){
	ctx->session_expires=expires;
	return ;
}
void sal_use_double_registrations(Sal *ctx, bool_t enabled){
	ms_warning("sal_use_double_registrations is deprecated");
	return ;
}
void sal_reuse_authorization(Sal *ctx, bool_t enabled){
	ms_warning("sal_reuse_authorization is deprecated");
	return ;
}
void sal_use_one_matching_codec_policy(Sal *ctx, bool_t one_matching_codec){
	ctx->one_matching_codec=one_matching_codec;
}
void sal_use_rport(Sal *ctx, bool_t use_rports){
	belle_sip_provider_enable_rport(ctx->prov,use_rports);
	ms_message("Sal use rport [%s]",use_rports?"enabled":"disabled");
	return ;
}
void sal_use_101(Sal *ctx, bool_t use_101){
	ms_warning("sal_use_101 is deprecated");
	return ;
}
void sal_set_root_ca(Sal* ctx, const char* rootCa){
	ms_error("sal_set_root_ca not implemented yet");
	return ;
}
void sal_verify_server_certificates(Sal *ctx, bool_t verify){
	ms_error("sal_verify_server_certificates not implemented yet");
	return ;
}
void sal_verify_server_cn(Sal *ctx, bool_t verify){
	ms_error("sal_verify_server_cn not implemented yet");
	return ;
}

void sal_use_tcp_tls_keepalive(Sal *ctx, bool_t enabled) {
	ctx->use_tcp_tls_keep_alive=enabled;
}

int sal_iterate(Sal *sal){
	belle_sip_stack_sleep(sal->stack,0);
	return 0;
}
MSList * sal_get_pending_auths(Sal *sal){
	return ms_list_copy(sal->pending_auths);
}

#define payload_type_set_number(pt,n)	(pt)->user_data=(void*)((long)n);
#define payload_type_get_number(pt)		((int)(long)(pt)->user_data)

/*misc*/
void sal_get_default_local_ip(Sal *sal, int address_family, char *ip, size_t iplen){
	strncpy(ip,address_family==AF_INET6 ? "::1" : "127.0.0.1",iplen);
	ms_error("Could not find default routable ip address !");
}

const char *sal_get_root_ca(Sal* ctx) {
	ms_fatal("sal_get_root_ca not implemented yet");
	return  NULL;
}
int sal_reset_transports(Sal *ctx){
	ms_message("reseting transport");
	belle_sip_provider_clean_channels(ctx->prov);
	return 0;
}
void sal_set_dscp(Sal *ctx, int dscp){
	ms_warning("sal_set_dscp not implemented");
}
void  sal_set_send_error(Sal *sal,int value) {
	 belle_sip_stack_set_send_error(sal->stack,value);
}
void  sal_set_recv_error(Sal *sal,int value) {
	 belle_sip_provider_set_recv_error(sal->prov,value);
}
void sal_nat_helper_enable(Sal *sal,bool_t enable) {
	sal->nat_helper_enabled=enable;
	ms_message("Sal nat helper [%s]",enable?"enabled":"disabled");
}
bool_t sal_nat_helper_enabled(Sal *sal) {
	return sal->nat_helper_enabled;
}
void sal_set_dns_timeout(Sal* sal,int timeout) {
	belle_sip_stack_set_dns_timeout(sal->stack, timeout);
}
int sal_get_dns_timeout(const Sal* sal)  {
	return belle_sip_stack_get_dns_timeout(sal->stack);
}