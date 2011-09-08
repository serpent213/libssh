/*
 * auth.c - Authentication with SSH protocols
 *
 * This file is part of the SSH Library
 *
 * Copyright (c) 2003-2011 by Aris Adamantiadis <aris@0xbadc0de.be>
 * Copyright (c) 2008-2011 Andreas Schneider <asn@cryptomilk.org>
 *
 * The SSH Library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * The SSH Library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the SSH Library; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#endif

#include "libssh/priv.h"
#include "libssh/ssh2.h"
#include "libssh/buffer.h"
#include "libssh/agent.h"
#include "libssh/misc.h"
#include "libssh/packet.h"
#include "libssh/session.h"
#include "libssh/keys.h"
#include "libssh/auth.h"
#include "libssh/pki.h"

#include "libssh/legacy.h"

/**
 * @defgroup libssh_auth The SSH authentication functions.
 * @ingroup libssh
 *
 * Functions to authenticate with a server.
 *
 * @{
 */

/**
 * @internal
 *
 * @brief Ask access to the ssh-userauth service.
 *
 * @param[in] session   The SSH session handle.
 *
 * @returns SSH_OK on success, SSH_ERROR on error.
 * @returns SSH_AGAIN on nonblocking mode, if calling that function
 * again is necessary
 */
static int ssh_userauth_request_service(ssh_session session) {
    return ssh_service_request(session,"ssh-userauth");
}

static int ssh_auth_response_termination(void *user){
  ssh_session session=(ssh_session)user;
  switch(session->auth_state){
    case SSH_AUTH_STATE_NONE:
    case SSH_AUTH_STATE_KBDINT_SENT:
      return 0;
    default:
      return 1;
  }
}

/**
 * @internal
 * @brief Wait for a response of an authentication function.
 *
 * @param[in] session   The SSH session.
 *
 * @returns SSH_AUTH_SUCCESS Authentication success, or pubkey accepted
 *          SSH_AUTH_PARTIAL Authentication succeeded but another mean
 *                           of authentication is needed.
 *          SSH_AUTH_INFO    Data for keyboard-interactive
 *          SSH_AUTH_AGAIN   In nonblocking mode, call has to be made again
 *          SSH_AUTH_ERROR   Error during the process.
 */
static int ssh_userauth_get_response(ssh_session session) {
    int rc = SSH_AUTH_ERROR;

    rc = ssh_handle_packets_termination(session, SSH_TIMEOUT_USER,
        ssh_auth_response_termination, session);
    if (rc == SSH_ERROR) {
      leave_function();
      return SSH_AUTH_ERROR;
    }
    if (!ssh_auth_response_termination(session)){
      leave_function();
      return SSH_AUTH_AGAIN;
    }

    switch(session->auth_state) {
        case SSH_AUTH_STATE_ERROR:
            rc = SSH_AUTH_ERROR;
            break;
        case SSH_AUTH_STATE_FAILED:
            rc = SSH_AUTH_DENIED;
            break;
        case SSH_AUTH_STATE_INFO:
            rc = SSH_AUTH_INFO;
            break;
        case SSH_AUTH_STATE_PARTIAL:
            rc = SSH_AUTH_PARTIAL;
            break;
        case SSH_AUTH_STATE_PK_OK:
        case SSH_AUTH_STATE_SUCCESS:
            rc = SSH_AUTH_SUCCESS;
            break;
        case SSH_AUTH_STATE_KBDINT_SENT:
        case SSH_AUTH_STATE_NONE:
            /* not reached */
            rc = SSH_AUTH_ERROR;
            break;
    }

    return rc;
}

/**
 * @internal
 *
 * @brief Handles a SSH_USERAUTH_BANNER packet.
 *
 * This banner should be shown to user prior to authentication
 */
SSH_PACKET_CALLBACK(ssh_packet_userauth_banner){
  ssh_string banner;
  (void)type;
  (void)user;
  enter_function();
  banner = buffer_get_ssh_string(packet);
  if (banner == NULL) {
    ssh_log(session, SSH_LOG_RARE,
        "Invalid SSH_USERAUTH_BANNER packet");
  } else {
    ssh_log(session, SSH_LOG_PACKET,
        "Received SSH_USERAUTH_BANNER packet");
    if(session->banner != NULL)
      ssh_string_free(session->banner);
    session->banner = banner;
  }
  leave_function();
  return SSH_PACKET_USED;
}

/**
 * @internal
 *
 * @brief Handles a SSH_USERAUTH_FAILURE packet.
 *
 * This handles the complete or partial authentication failure.
 */
SSH_PACKET_CALLBACK(ssh_packet_userauth_failure){
  char *auth_methods = NULL;
  ssh_string auth;
  uint8_t partial = 0;
  (void) type;
  (void) user;
  enter_function();

  auth = buffer_get_ssh_string(packet);
  if (auth == NULL || buffer_get_u8(packet, &partial) != 1) {
    ssh_set_error(session, SSH_FATAL,
        "Invalid SSH_MSG_USERAUTH_FAILURE message");
    session->auth_state=SSH_AUTH_STATE_ERROR;
    goto end;
  }

  auth_methods = ssh_string_to_char(auth);
  if (auth_methods == NULL) {
    ssh_set_error_oom(session);
    goto end;
  }

  if (partial) {
    session->auth_state=SSH_AUTH_STATE_PARTIAL;
    ssh_log(session,SSH_LOG_PROTOCOL,
        "Partial success. Authentication that can continue: %s",
        auth_methods);
  } else {
    session->auth_state=SSH_AUTH_STATE_FAILED;
    ssh_log(session, SSH_LOG_PROTOCOL,
        "Access denied. Authentication that can continue: %s",
        auth_methods);
    ssh_set_error(session, SSH_REQUEST_DENIED,
            "Access denied. Authentication that can continue: %s",
            auth_methods);

    session->auth_methods = 0;
  }
  if (strstr(auth_methods, "password") != NULL) {
    session->auth_methods |= SSH_AUTH_METHOD_PASSWORD;
  }
  if (strstr(auth_methods, "keyboard-interactive") != NULL) {
    session->auth_methods |= SSH_AUTH_METHOD_INTERACTIVE;
  }
  if (strstr(auth_methods, "publickey") != NULL) {
    session->auth_methods |= SSH_AUTH_METHOD_PUBLICKEY;
  }
  if (strstr(auth_methods, "hostbased") != NULL) {
    session->auth_methods |= SSH_AUTH_METHOD_HOSTBASED;
  }

end:
  ssh_string_free(auth);
  SAFE_FREE(auth_methods);
  leave_function();
  return SSH_PACKET_USED;
}

/**
 * @internal
 *
 * @brief Handles a SSH_USERAUTH_SUCCESS packet.
 *
 * It is also used to communicate the new to the upper levels.
 */
SSH_PACKET_CALLBACK(ssh_packet_userauth_success){
  enter_function();
  (void)packet;
  (void)type;
  (void)user;
  ssh_log(session,SSH_LOG_PACKET,"Received SSH_USERAUTH_SUCCESS");
  ssh_log(session,SSH_LOG_PROTOCOL,"Authentication successful");
  session->auth_state=SSH_AUTH_STATE_SUCCESS;
  session->session_state=SSH_SESSION_STATE_AUTHENTICATED;
  if(session->current_crypto && session->current_crypto->delayed_compress_out){
  	ssh_log(session,SSH_LOG_PROTOCOL,"Enabling delayed compression OUT");
  	session->current_crypto->do_compress_out=1;
  }
  if(session->current_crypto && session->current_crypto->delayed_compress_in){
  	ssh_log(session,SSH_LOG_PROTOCOL,"Enabling delayed compression IN");
  	session->current_crypto->do_compress_in=1;
  }
  leave_function();
  return SSH_PACKET_USED;
}

/**
 * @internal
 *
 * @brief Handles a SSH_USERAUTH_PK_OK or SSH_USERAUTH_INFO_REQUEST packet.
 *
 * Since the two types of packets share the same code, additional work is done
 * to understand if we are in a public key or keyboard-interactive context.
 */
SSH_PACKET_CALLBACK(ssh_packet_userauth_pk_ok){
	int rc;
	enter_function();
  ssh_log(session,SSH_LOG_PACKET,"Received SSH_USERAUTH_PK_OK/INFO_REQUEST");
  if(session->auth_state==SSH_AUTH_STATE_KBDINT_SENT){
    /* Assuming we are in keyboard-interactive context */
    ssh_log(session,SSH_LOG_PACKET,"keyboard-interactive context, assuming SSH_USERAUTH_INFO_REQUEST");
    rc=ssh_packet_userauth_info_request(session,type,packet,user);
  } else {
    session->auth_state=SSH_AUTH_STATE_PK_OK;
    ssh_log(session,SSH_LOG_PACKET,"assuming SSH_USERAUTH_PK_OK");
    rc=SSH_PACKET_USED;
  }
  leave_function();
  return rc;
}

/**
 * @brief Get available authentication methods from the server.
 *
 * This requires the function ssh_userauth_none() to be called before the
 * methods are available. The server MAY return a list of methods that may
 * continue.
 *
 * @param[in] session   The SSH session.
 *
 * @param[in] username  Deprecated, set to NULL.
 *
 * @returns             A bitfield of the fllowing values:
 *                      - SSH_AUTH_METHOD_PASSWORD
 *                      - SSH_AUTH_METHOD_PUBLICKEY
 *                      - SSH_AUTH_METHOD_HOSTBASED
 *                      - SSH_AUTH_METHOD_INTERACTIVE
 *
 * @warning Other reserved flags may appear in future versions.
 * @see ssh_userauth_none()
 */
int ssh_userauth_list(ssh_session session, const char *username)
{
    (void) username; /* unused */

    if (session == NULL) {
        return 0;
    }

#ifdef WITH_SSH1
    if(session->version == 1) {
        return SSH_AUTH_METHOD_PASSWORD;
    }
#endif

    return session->auth_methods;
}

/**
 * @brief Try to authenticate through the "none" method.
 *
 * @param[in] session   The ssh session to use.
 *
 * @param[in] username    The username, this SHOULD be NULL.
 *
 * @returns SSH_AUTH_ERROR:   A serious error happened.\n
 *          SSH_AUTH_DENIED:  Authentication failed: use another method\n
 *          SSH_AUTH_PARTIAL: You've been partially authenticated, you still
 *                            have to use another method\n
 *          SSH_AUTH_SUCCESS: Authentication success\n
 *          SSH_AUTH_AGAIN:   In nonblocking mode, you've got to call this again
 *                            later.
 *
 * @note Most server implementations do not permit changing the username during
 * authentication. The username should only be set with ssh_optoins_set() only
 * before you connect to the server.
 */
int ssh_userauth_none(ssh_session session, const char *username) {
    ssh_string str;
    int rc;

#ifdef WITH_SSH1
    if (session->version == 1) {
        return ssh_userauth1_none(session, username);
    }
#endif

    switch(session->pending_call_state){
        case SSH_PENDING_CALL_NONE:
            break;
        case SSH_PENDING_CALL_AUTH_NONE:
            goto pending;
        default:
            ssh_set_error(session,SSH_FATAL,"Bad call during pending SSH call in ssh_userauth_none");
            return SSH_AUTH_ERROR;
    }

    rc = ssh_userauth_request_service(session);
    if (rc == SSH_AGAIN) {
        return SSH_AUTH_AGAIN;
    } else if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

    /* request */
    rc = buffer_add_u8(session->out_buffer, SSH2_MSG_USERAUTH_REQUEST);
    if (rc < 0) {
        goto fail;
    }

    /* username */
    if (username) {
        str = ssh_string_from_char(username);
    } else {
        str = ssh_string_from_char(session->username);
    }
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* service */
    str = ssh_string_from_char("ssh-connection");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* method */
    str = ssh_string_from_char("none");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    session->auth_state = SSH_AUTH_STATE_NONE;
    session->pending_call_state = SSH_PENDING_CALL_AUTH_NONE;
    rc = packet_send(session);
    if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

pending:
    rc = ssh_userauth_get_response(session);
    if (rc != SSH_AUTH_AGAIN) {
        session->pending_call_state = SSH_PENDING_CALL_NONE;
    }

    return rc;
fail:
    ssh_set_error_oom(session);
    buffer_reinit(session->out_buffer);

    return SSH_AUTH_ERROR;
}

/**
 * @brief Try to authenticate with the given public key.
 *
 * To avoid unnecessary processing and user interaction, the following method
 * is provided for querying whether authentication using the 'pubkey' would
 * be possible.
 *
 * @param[in] session     The SSH session.
 *
 * @param[in] username    The username, this SHOULD be NULL.
 *
 * @param[in] pubkey      The public key to try.
 *
 * @return  SSH_AUTH_ERROR:   A serious error happened.\n
 *          SSH_AUTH_DENIED:  The server doesn't accept that public key as an
 *                            authentication token. Try another key or another
 *                            method.\n
 *          SSH_AUTH_PARTIAL: You've been partially authenticated, you still
 *                            have to use another method.\n
 *          SSH_AUTH_SUCCESS: The public key is accepted, you want now to use
 *                            ssh_userauth_pubkey().
 *          SSH_AUTH_AGAIN:   In nonblocking mode, you've got to call this again
 *                            later.
 *
 * @note Most server implementations do not permit changing the username during
 * authentication. The username should only be set with ssh_optoins_set() only
 * before you connect to the server.
 */
int ssh_userauth_try_publickey(ssh_session session,
                               const char *username,
                               const ssh_key pubkey)
{
    ssh_string str;
    int rc;

    if (session == NULL) {
        return SSH_AUTH_ERROR;
    }

    if (pubkey == NULL || !ssh_key_is_public(pubkey)) {
        ssh_set_error(session, SSH_FATAL, "Invalid pubkey");
        return SSH_AUTH_ERROR;
    }

#ifdef WITH_SSH1
    if (session->version == 1) {
        return SSH_AUTH_DENIED;
    }
#endif

    switch(session->pending_call_state) {
        case SSH_PENDING_CALL_NONE:
            break;
        case SSH_PENDING_CALL_AUTH_OFFER_PUBKEY:
            goto pending;
        default:
            ssh_set_error(session,
                          SSH_FATAL,
                          "Bad call during pending SSH call in ssh_userauth_try_pubkey");
            return SSH_ERROR;
    }

    rc = ssh_userauth_request_service(session);
    if (rc == SSH_AGAIN) {
        return SSH_AUTH_AGAIN;
    } else if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

    /* request */
    rc = buffer_add_u8(session->out_buffer, SSH2_MSG_USERAUTH_REQUEST);
    if (rc < 0) {
        goto fail;
    }

    /* username */
    if (username) {
        str = ssh_string_from_char(username);
    } else {
        str = ssh_string_from_char(session->username);
    }
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* service */
    str = ssh_string_from_char("ssh-connection");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* method */
    str = ssh_string_from_char("publickey");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* private key? */
    rc = buffer_add_u8(session->out_buffer, 0);
    if (rc < 0) {
        goto fail;
    }

    /* algo */
    str = ssh_string_from_char(pubkey->type_c);
    if (rc < 0) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* public key */
    rc = ssh_pki_export_pubkey_blob(pubkey, &str);
    if (rc < 0) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    session->auth_state = SSH_AUTH_STATE_NONE;
    session->pending_call_state = SSH_PENDING_CALL_AUTH_OFFER_PUBKEY;
    rc = packet_send(session);
    if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

pending:
    rc = ssh_userauth_get_response(session);
    if (rc != SSH_AUTH_AGAIN) {
        session->pending_call_state = SSH_PENDING_CALL_NONE;
    }

    return rc;
fail:
    ssh_set_error_oom(session);
    buffer_reinit(session->out_buffer);

    return SSH_AUTH_ERROR;
}

/**
 * @brief Authenticate with public/private key.
 *
 * @param[in] session     The SSH session.
 *
 * @param[in] username    The username, this SHOULD be NULL.
 *
 * @param[in] privkey     The private key for authentication.
 *
 * @return  SSH_AUTH_ERROR:   A serious error happened.\n
 *          SSH_AUTH_DENIED:  The server doesn't accept that public key as an
 *                            authentication token. Try another key or another
 *                            method.\n
 *          SSH_AUTH_PARTIAL: You've been partially authenticated, you still
 *                            have to use another method.\n
 *          SSH_AUTH_SUCCESS: The public key is accepted, you want now to use
 *                            ssh_userauth_pubkey().
 *          SSH_AUTH_AGAIN:   In nonblocking mode, you've got to call this again
 *                            later.
 *
 * @note Most server implementations do not permit changing the username during
 * authentication. The username should only be set with ssh_optoins_set() only
 * before you connect to the server.
 */
int ssh_userauth_publickey(ssh_session session,
                           const char *username,
                           const ssh_key privkey)
{
    ssh_string str;
    int rc;

    if (session == NULL) {
        return SSH_AUTH_ERROR;
    }

    if (privkey == NULL || !ssh_key_is_private(privkey)) {
        ssh_set_error(session, SSH_FATAL, "Invalid private key");
        return SSH_AUTH_ERROR;
    }

#ifdef WITH_SSH1
    if (session->version == 1) {
        return SSH_AUTH_DENIED;
    }
#endif

    switch(session->pending_call_state) {
        case SSH_PENDING_CALL_NONE:
            break;
        case SSH_PENDING_CALL_AUTH_PUBKEY:
            goto pending;
        default:
            ssh_set_error(session,
                          SSH_FATAL,
                          "Bad call during pending SSH call in ssh_userauth_try_pubkey");
            return SSH_AUTH_ERROR;
    }

    rc = ssh_userauth_request_service(session);
    if (rc == SSH_AGAIN) {
        return SSH_AUTH_AGAIN;
    } else if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

    /* request */
    rc = buffer_add_u8(session->out_buffer, SSH2_MSG_USERAUTH_REQUEST);
    if (rc < 0) {
        goto fail;
    }

    /* username */
    if (username) {
        str = ssh_string_from_char(username);
    } else {
        str = ssh_string_from_char(session->username);
    }
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* service */
    str = ssh_string_from_char("ssh-connection");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* method */
    str = ssh_string_from_char("publickey");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* private key? */
    rc = buffer_add_u8(session->out_buffer, 1);
    if (rc < 0) {
        goto fail;
    }

    /* algo */
    str = ssh_string_from_char(privkey->type_c);
    if (rc < 0) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* public key */
    rc = ssh_pki_export_pubkey_blob(privkey, &str);
    if (rc < 0) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* sign the buffer with the private key */
    str = ssh_pki_do_sign(session, session->out_buffer, privkey);
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    session->auth_state = SSH_AUTH_STATE_NONE;
    session->pending_call_state = SSH_PENDING_CALL_AUTH_PUBKEY;
    rc = packet_send(session);
    if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

pending:
    rc = ssh_userauth_get_response(session);
    if (rc != SSH_AUTH_AGAIN) {
        session->pending_call_state = SSH_PENDING_CALL_NONE;
    }

    return rc;
fail:
    ssh_set_error_oom(session);
    buffer_reinit(session->out_buffer);

    return SSH_AUTH_ERROR;
}

#ifndef _WIN32
static int ssh_userauth_agent_publickey(ssh_session session,
                                        const char *username,
                                        ssh_key pubkey)
{
    ssh_string str;
    int rc;

    switch(session->pending_call_state) {
        case SSH_PENDING_CALL_NONE:
            break;
        case SSH_PENDING_CALL_AUTH_AGENT:
            goto pending;
        default:
            ssh_set_error(session,
                          SSH_FATAL,
                          "Bad call during pending SSH call in ssh_userauth_try_pubkey");
            return SSH_ERROR;
    }

    rc = ssh_userauth_request_service(session);
    if (rc == SSH_AGAIN) {
        return SSH_AUTH_AGAIN;
    } else if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

    /* request */
    rc = buffer_add_u8(session->out_buffer, SSH2_MSG_USERAUTH_REQUEST);
    if (rc < 0) {
        goto fail;
    }

    /* username */
    if (username) {
        str = ssh_string_from_char(username);
    } else {
        str = ssh_string_from_char(session->username);
    }
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* service */
    str = ssh_string_from_char("ssh-connection");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* method */
    str = ssh_string_from_char("publickey");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* private key? */
    rc = buffer_add_u8(session->out_buffer, 1);
    if (rc < 0) {
        goto fail;
    }

    /* algo */
    str = ssh_string_from_char(pubkey->type_c);
    if (rc < 0) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* public key */
    rc = ssh_pki_export_pubkey_blob(pubkey, &str);
    if (rc < 0) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* sign the buffer with the private key */
    str = ssh_pki_do_sign_agent(session, session->out_buffer, pubkey);
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    session->auth_state = SSH_AUTH_STATE_NONE;
    session->pending_call_state = SSH_PENDING_CALL_AUTH_AGENT;
    rc = packet_send(session);
    if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

pending:
    rc = ssh_userauth_get_response(session);
    if (rc != SSH_AUTH_AGAIN) {
        session->pending_call_state = SSH_PENDING_CALL_NONE;
    }

    return rc;
fail:
    ssh_set_error_oom(session);
    buffer_reinit(session->out_buffer);

    return SSH_AUTH_ERROR;
}

/**
 * @brief Try to do public key authentication with ssh agent.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @param[in]  username The username, this SHOULD be NULL.
 *
 * @return  SSH_AUTH_ERROR:   A serious error happened.\n
 *          SSH_AUTH_DENIED:  The server doesn't accept that public key as an
 *                            authentication token. Try another key or another
 *                            method.\n
 *          SSH_AUTH_PARTIAL: You've been partially authenticated, you still
 *                            have to use another method.\n
 *          SSH_AUTH_SUCCESS: The public key is accepted, you want now to use
 *                            ssh_userauth_pubkey().
 *          SSH_AUTH_AGAIN:   In nonblocking mode, you've got to call this again
 *                            later.
 *
 * @note Most server implementations do not permit changing the username during
 * authentication. The username should only be set with ssh_optoins_set() only
 * before you connect to the server.
 */
int ssh_userauth_agent(ssh_session session,
                       const char *username)
{
    ssh_key pubkey;
    char *comment;
    int rc;

    if (session == NULL) {
        return SSH_AUTH_ERROR;
    }

    if (!agent_is_running(session)) {
        return SSH_AUTH_DENIED;
    }

    for (pubkey = ssh_agent_get_first_ident(session, &comment);
         pubkey != NULL;
         pubkey = ssh_agent_get_next_ident(session, &comment)) {
        ssh_log(session, SSH_LOG_RARE, "Trying identity %s", comment);

        rc = ssh_userauth_try_publickey(session, username, pubkey);
        if (rc == SSH_AUTH_ERROR) {
            ssh_string_free_char(comment);
            ssh_key_free(pubkey);
            return rc;
        } else if (rc != SSH_AUTH_SUCCESS) {
            ssh_log(session, SSH_LOG_PROTOCOL, "Public key of %s refused by server", comment);
            ssh_string_free_char(comment);
            ssh_key_free(pubkey);
            continue;
        }

        ssh_log(session, SSH_LOG_PROTOCOL, "Public key of %s accepted by server", comment);

        rc = ssh_userauth_agent_publickey(session, username, pubkey);
        ssh_string_free_char(comment);
        ssh_key_free(pubkey);
        if (rc == SSH_AUTH_ERROR) {
            return rc;
        } else if (rc != SSH_AUTH_SUCCESS) {
            ssh_log(session,
                    SSH_LOG_RARE,
                    "Server accepted public key but refused the signature");
            continue;
        }

        return SSH_AUTH_SUCCESS;
    }

    return SSH_AUTH_ERROR;
}
#endif

/**
 * @brief Tries to automatically authenticate with public key and "none"
 *
 * It may fail, for instance it doesn't ask for a password and uses a default
 * asker for passphrases (in case the private key is encrypted).
 *
 * @param[in]  session     The SSH session.
 *
 * @param[in]  username    The username, this SHOULD be NULL.
 *
 * @param[in]  passphrase  Use this passphrase to unlock the privatekey. Use NULL
 *                         if you don't want to use a passphrase or the user
 *                         should be asked.
 *
 * @return  SSH_AUTH_ERROR:   A serious error happened.\n
 *          SSH_AUTH_DENIED:  The server doesn't accept that public key as an
 *                            authentication token. Try another key or another
 *                            method.\n
 *          SSH_AUTH_PARTIAL: You've been partially authenticated, you still
 *                            have to use another method.\n
 *          SSH_AUTH_SUCCESS: The public key is accepted, you want now to use
 *                            ssh_userauth_pubkey().
 *          SSH_AUTH_AGAIN:   In nonblocking mode, you've got to call this again
 *                            later.
 *
 * @note Most server implementations do not permit changing the username during
 * authentication. The username should only be set with ssh_optoins_set() only
 * before you connect to the server.
 */
int ssh_userauth_publickey_auto(ssh_session session,
                                const char *username,
                                const char *passphrase)
{
    struct ssh_iterator *it;
    ssh_auth_callback auth_fn = NULL;
    void *auth_data = NULL;
    int rc;

    if (session == NULL) {
        return SSH_AUTH_ERROR;
    }

    if (session->common.callbacks) {
        auth_fn = session->common.callbacks->auth_function;
        auth_data = session->common.callbacks->userdata;
    }

#ifndef _WIN32
    /* Try authentication with ssh-agent first */
    rc = ssh_userauth_agent(session, username);
    if (rc == SSH_AUTH_ERROR || rc == SSH_AUTH_SUCCESS) {
        return rc;
    }
#endif

    for (it = ssh_list_get_iterator(session->identity);
            it != NULL;
            it = it->next) {
        const char *privkey_file = it->data;
        char pubkey_file[1024] = {0};
        ssh_key privkey = NULL;
        ssh_key pubkey;

        ssh_log(session,
                SSH_LOG_PROTOCOL,
                "Trying to authenticate with %s",
                privkey_file);

        snprintf(pubkey_file, sizeof(pubkey_file), "%s.pub", privkey_file);

        rc = ssh_pki_import_pubkey_file(pubkey_file, &pubkey);
        if (rc == SSH_ERROR) {
            ssh_set_error(session,
                          SSH_FATAL,
                          "Failed to import public key: %s",
                          pubkey_file);
            return SSH_AUTH_ERROR;
        } else if (rc == SSH_EOF) {
            /* Read the private key and save the public key to file */
            rc = ssh_pki_import_privkey_file(privkey_file,
                                             passphrase,
                                             auth_fn,
                                             auth_data,
                                             &privkey);
            if (rc == SSH_ERROR) {
                ssh_set_error(session,
                              SSH_FATAL,
                              "Failed to read private key: %s",
                              privkey_file);
                continue;
                return SSH_AUTH_ERROR;
            } else if (rc == SSH_EOF) {
                /* If the file doesn't exist, continue */
                ssh_log(session,
                        SSH_LOG_PACKET,
                        "Private key %s doesn't exist.",
                        privkey_file);
                continue;
            }

            rc = ssh_pki_export_privkey_to_pubkey(privkey, &pubkey);
            if (rc == SSH_ERROR) {
                ssh_key_free(privkey);
                return SSH_AUTH_ERROR;
            }

            rc = ssh_pki_export_pubkey_file(pubkey, pubkey_file);
            if (rc == SSH_ERROR) {
                ssh_log(session,
                        SSH_LOG_PACKET,
                        "Could not write public key to file: %s",
                        pubkey_file);
            }
        }

        rc = ssh_userauth_try_publickey(session, username, pubkey);
        if (rc == SSH_AUTH_ERROR) {
            ssh_log(session,
                    SSH_LOG_RARE,
                    "Public key authentication error for %s",
                    privkey_file);
            ssh_key_free(privkey);
            ssh_key_free(pubkey);
            return rc;
        } else if (rc != SSH_AUTH_SUCCESS) {
            ssh_log(session,
                    SSH_LOG_PROTOCOL,
                    "Public key for %s refused by server",
                    privkey_file);
            ssh_key_free(privkey);
            ssh_key_free(pubkey);
            continue;
        }

        /* Public key has been accepted by the server */
        if (privkey == NULL) {
            rc = ssh_pki_import_privkey_file(privkey_file,
                                             passphrase,
                                             auth_fn,
                                             auth_data,
                                             &privkey);
            if (rc == SSH_ERROR) {
                ssh_key_free(pubkey);
                ssh_set_error(session,
                              SSH_FATAL,
                              "Failed to read private key: %s",
                              privkey_file);
                continue;
            } else if (rc == SSH_EOF) {
                /* If the file doesn't exist, continue */
                ssh_key_free(pubkey);
                ssh_log(session,
                        SSH_LOG_PACKET,
                        "Private key %s doesn't exist.",
                        privkey_file);
                continue;
            }
        }

        rc = ssh_userauth_publickey(session, username, privkey);
        ssh_key_free(privkey);
        ssh_key_free(pubkey);
        if (rc == SSH_AUTH_ERROR) {
            return rc;
        } else if (rc == SSH_AUTH_SUCCESS) {
            ssh_log(session,
                    SSH_LOG_PROTOCOL,
                    "Successfully authenticated using %s",
                    privkey_file);
            return rc;
        }

        ssh_log(session,
                SSH_LOG_RARE,
                "The server accepted the public key but refused the signature");
        /* continue */
    }
    ssh_log(session,
            SSH_LOG_PROTOCOL,
            "Tried every public key, none matched");

    return SSH_AUTH_DENIED;
}

/**
 * @brief Try to authenticate by password.
 *
 * This authentication method is normally disabled on SSHv2 server. You should
 * use keyboard-interactive mode.
 *
 * The 'password' value MUST be encoded UTF-8.  It is up to the server how to
 * interpret the password and validate it against the password database.
 * However, if you read the password in some other encoding, you MUST convert
 * the password to UTF-8.
 *
 * @param[in] session   The ssh session to use.
 *
 * @param[in] username  The username, this SHOULD be NULL.
 *
 * @param[in] password  The password to authenticate in UTF-8.
 *
 * @returns             A bitfield of the fllowing values:
 *                      - SSH_AUTH_METHOD_PASSWORD
 *                      - SSH_AUTH_METHOD_PUBLICKEY
 *                      - SSH_AUTH_METHOD_HOSTBASED
 *                      - SSH_AUTH_METHOD_INTERACTIVE
 *
 * @note Most server implementations do not permit changing the username during
 * authentication. The username should only be set with ssh_optoins_set() only
 * before you connect to the server.
 *
 * @see ssh_userauth_none()
 * @see ssh_userauth_kbdint()
 */
int ssh_userauth_password(ssh_session session,
                          const char *username,
                          const char *password) {
    ssh_string str;
    int rc;

#ifdef WITH_SSH1
    if (session->version == 1) {
        rc = ssh_userauth1_password(session, username, password);
        return rc;
    }
#endif

    switch(session->pending_call_state) {
        case SSH_PENDING_CALL_NONE:
            break;
        case SSH_PENDING_CALL_AUTH_OFFER_PUBKEY:
            goto pending;
        default:
            ssh_set_error(session,
                          SSH_FATAL,
                          "Bad call during pending SSH call in ssh_userauth_try_pubkey");
            return SSH_ERROR;
    }

    rc = ssh_userauth_request_service(session);
    if (rc == SSH_AGAIN) {
        return SSH_AUTH_AGAIN;
    } else if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

    /* request */
    rc = buffer_add_u8(session->out_buffer, SSH2_MSG_USERAUTH_REQUEST);
    if (rc < 0) {
        goto fail;
    }

    /* username */
    if (username) {
        str = ssh_string_from_char(username);
    } else {
        str = ssh_string_from_char(session->username);
    }
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* service */
    str = ssh_string_from_char("ssh-connection");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* method */
    str = ssh_string_from_char("password");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* FALSE */
    rc = buffer_add_u8(session->out_buffer, 0);
    if (rc < 0) {
        goto fail;
    }

    /* password */
    str = ssh_string_from_char(password);
    if (rc < 0) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    session->auth_state = SSH_AUTH_STATE_NONE;
    session->pending_call_state = SSH_PENDING_CALL_AUTH_OFFER_PUBKEY;
    rc = packet_send(session);
    if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

pending:
    rc = ssh_userauth_get_response(session);
    if (rc != SSH_AUTH_AGAIN) {
        session->pending_call_state = SSH_PENDING_CALL_NONE;
    }

    return rc;
fail:
    ssh_set_error_oom(session);
    buffer_reinit(session->out_buffer);

    return SSH_AUTH_ERROR;
}

#ifndef _WIN32
/* LEGACY */
int ssh_userauth_agent_pubkey(ssh_session session,
                              const char *username,
                              ssh_public_key publickey)
{
    ssh_key key;
    int rc;

    key = ssh_key_new();
    if (key == NULL) {
        return SSH_AUTH_ERROR;
    }

    key->type = publickey->type;
    key->type_c = ssh_key_type_to_char(key->type);
    key->flags = SSH_KEY_FLAG_PUBLIC;
    key->dsa = publickey->dsa_pub;
    key->rsa = publickey->rsa_pub;

    rc = ssh_userauth_agent_publickey(session, username, key);

    key->dsa = NULL;
    key->rsa = NULL;
    ssh_key_free(key);

    return rc;
}
#endif /* _WIN32 */

ssh_kbdint ssh_kbdint_new(void) {
    ssh_kbdint kbd;

    kbd = malloc(sizeof(struct ssh_kbdint_struct));
    if (kbd == NULL) {
        return NULL;
    }
    ZERO_STRUCTP(kbd);

    return kbd;
}


void ssh_kbdint_free(ssh_kbdint kbd) {
    int i, n;

    if (kbd == NULL) {
        return;
    }

    SAFE_FREE(kbd->name);
    SAFE_FREE(kbd->instruction);
    SAFE_FREE(kbd->echo);

    n = kbd->nprompts;
    if (kbd->prompts) {
        for (i = 0; i < n; i++) {
            BURN_STRING(kbd->prompts[i]);
            SAFE_FREE(kbd->prompts[i]);
        }
        SAFE_FREE(kbd->prompts);
    }

    n = kbd->nanswers;
    if (kbd->answers) {
        for (i = 0; i < n; i++) {
            BURN_STRING(kbd->answers[i]);
            SAFE_FREE(kbd->answers[i]);
        }
        SAFE_FREE(kbd->answers);
    }

    SAFE_FREE(kbd);
}

void ssh_kbdint_clean(ssh_kbdint kbd) {
    int i, n;

    if (kbd == NULL) {
        return;
    }

    SAFE_FREE(kbd->name);
    SAFE_FREE(kbd->instruction);
    SAFE_FREE(kbd->echo);

    n = kbd->nprompts;
    if (kbd->prompts) {
        for (i = 0; i < n; i++) {
            BURN_STRING(kbd->prompts[i]);
            SAFE_FREE(kbd->prompts[i]);
        }
        SAFE_FREE(kbd->prompts);
    }

    n = kbd->nanswers;

    if (kbd->answers) {
        for (i = 0; i < n; i++) {
            BURN_STRING(kbd->answers[i]);
            SAFE_FREE(kbd->answers[i]);
        }
        SAFE_FREE(kbd->answers);
    }

    kbd->nprompts = 0;
    kbd->nanswers = 0;
}

/*
 * This function sends the first packet as explained in RFC 3066 section 3.1.
 */
static int ssh_userauth_kbdint_init(ssh_session session,
                                    const char *username,
                                    const char *submethods)
{
    ssh_string str;
    int rc;

    rc = ssh_userauth_request_service(session);
    if (rc != SSH_OK) {
        return SSH_AUTH_ERROR;
    }

    /* request */
    rc = buffer_add_u8(session->out_buffer, SSH2_MSG_USERAUTH_REQUEST);
    if (rc < 0) {
        goto fail;
    }

    /* username */
    if (username) {
        str = ssh_string_from_char(username);
    } else {
        str = ssh_string_from_char(session->username);
    }
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* service */
    str = ssh_string_from_char("ssh-connection");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* method */
    str = ssh_string_from_char("keyboard-interactive");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* lang string (ignore it) */
    str = ssh_string_from_char("");
    if (str == NULL) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    /* submethods */
    if (submethods == NULL) {
        submethods = "";
    }

    str = ssh_string_from_char(submethods);
    if (rc < 0) {
        goto fail;
    }

    rc = buffer_add_ssh_string(session->out_buffer, str);
    ssh_string_free(str);
    if (rc < 0) {
        goto fail;
    }

    session->auth_state = SSH_AUTH_STATE_KBDINT_SENT;
    rc = packet_send(session);
    if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

    rc = ssh_userauth_get_response(session);

    return rc;
fail:
    ssh_set_error_oom(session);
    buffer_reinit(session->out_buffer);

    return SSH_AUTH_ERROR;
}

/**
 * @internal
 *
 * @brief Send the current challenge response and wait for a reply from the
 *        server.
 *
 * @returns SSH_AUTH_INFO if more info is needed
 * @returns SSH_AUTH_SUCCESS
 * @returns SSH_AUTH_FAILURE
 * @returns SSH_AUTH_PARTIAL
 */
static int ssh_userauth_kbdint_send(ssh_session session)
{
    ssh_string answer;
    uint32_t i;
    int rc;

    rc = buffer_add_u8(session->out_buffer, SSH2_MSG_USERAUTH_INFO_RESPONSE);
    if (rc < 0) {
        goto fail;
    }

    rc = buffer_add_u32(session->out_buffer, htonl(session->kbdint->nprompts));
    if (rc < 0) {
        goto fail;
    }

    for (i = 0; i < session->kbdint->nprompts; i++) {
        if (session->kbdint->answers && session->kbdint->answers[i]) {
            answer = ssh_string_from_char(session->kbdint->answers[i]);
        } else {
            answer = ssh_string_from_char("");
        }
        if (answer == NULL) {
            goto fail;
        }

        rc = buffer_add_ssh_string(session->out_buffer, answer);
        string_burn(answer);
        string_free(answer);
        if (rc < 0) {
            goto fail;
        }
    }

    session->auth_state = SSH_AUTH_STATE_KBDINT_SENT;
    ssh_kbdint_free(session->kbdint);
    session->kbdint = NULL;

    rc = packet_send(session);
    if (rc == SSH_ERROR) {
        return SSH_AUTH_ERROR;
    }

    rc = ssh_userauth_get_response(session);

    return rc;
fail:
    ssh_set_error_oom(session);
    buffer_reinit(session->out_buffer);

    return SSH_AUTH_ERROR;
}

/**
 * @internal
 * @brief handles a SSH_USERAUTH_INFO_REQUEST packet, as used in
 *        keyboard-interactive authentication, and changes the
 *        authentication state.
 */
SSH_PACKET_CALLBACK(ssh_packet_userauth_info_request) {
  ssh_string name; /* name of the "asking" window showed to client */
  ssh_string instruction;
  ssh_string tmp;
  uint32_t nprompts;
  uint32_t i;
  (void)user;
  (void)type;
  enter_function();

  name = buffer_get_ssh_string(packet);
  instruction = buffer_get_ssh_string(packet);
  tmp = buffer_get_ssh_string(packet);
  buffer_get_u32(packet, &nprompts);

  if (name == NULL || instruction == NULL || tmp == NULL) {
    ssh_string_free(name);
    ssh_string_free(instruction);
    /* tmp if empty if we got here */
    ssh_set_error(session, SSH_FATAL, "Invalid USERAUTH_INFO_REQUEST msg");
    leave_function();
    return SSH_PACKET_USED;
  }
  ssh_string_free(tmp);

  if (session->kbdint == NULL) {
    session->kbdint = ssh_kbdint_new();
    if (session->kbdint == NULL) {
      ssh_set_error_oom(session);
      ssh_string_free(name);
      ssh_string_free(instruction);

      leave_function();
      return SSH_PACKET_USED;
    }
  } else {
    ssh_kbdint_clean(session->kbdint);
  }

  session->kbdint->name = ssh_string_to_char(name);
  ssh_string_free(name);
  if (session->kbdint->name == NULL) {
    ssh_set_error_oom(session);
    ssh_kbdint_free(session->kbdint);
    leave_function();
    return SSH_PACKET_USED;
  }

  session->kbdint->instruction = ssh_string_to_char(instruction);
  ssh_string_free(instruction);
  if (session->kbdint->instruction == NULL) {
    ssh_set_error_oom(session);
    ssh_kbdint_free(session->kbdint);
    session->kbdint = NULL;
    leave_function();
    return SSH_PACKET_USED;
  }

  nprompts = ntohl(nprompts);
  ssh_log(session,SSH_LOG_PACKET,"kbdint: %d prompts",nprompts);
  if (nprompts == 0 ||
      nprompts > KBDINT_MAX_PROMPT) {
    ssh_set_error(session, SSH_FATAL,
        "Wrong number of prompts requested by the server: %u (0x%.4x)",
        nprompts, nprompts);
    ssh_kbdint_free(session->kbdint);
    session->kbdint = NULL;
    leave_function();
    return SSH_PACKET_USED;
  }

  session->kbdint->nprompts = nprompts;
  session->kbdint->nanswers = nprompts;
  session->kbdint->prompts = malloc(nprompts * sizeof(char *));
  if (session->kbdint->prompts == NULL) {
    session->kbdint->nprompts = 0;
    ssh_set_error_oom(session);
    ssh_kbdint_free(session->kbdint);
    session->kbdint = NULL;
    leave_function();
    return SSH_PACKET_USED;
  }
  memset(session->kbdint->prompts, 0, nprompts * sizeof(char *));

  session->kbdint->echo = malloc(nprompts);
  if (session->kbdint->echo == NULL) {
    session->kbdint->nprompts = 0;
    ssh_set_error_oom(session);
    ssh_kbdint_free(session->kbdint);
    session->kbdint = NULL;
    leave_function();
    return SSH_PACKET_USED;
  }
  memset(session->kbdint->echo, 0, nprompts);

  for (i = 0; i < nprompts; i++) {
    tmp = buffer_get_ssh_string(packet);
    buffer_get_u8(packet, &session->kbdint->echo[i]);
    if (tmp == NULL) {
      ssh_set_error(session, SSH_FATAL, "Short INFO_REQUEST packet");
      ssh_kbdint_free(session->kbdint);
      session->kbdint = NULL;
      leave_function();
      return SSH_PACKET_USED;
    }
    session->kbdint->prompts[i] = ssh_string_to_char(tmp);
    ssh_string_free(tmp);
    if (session->kbdint->prompts[i] == NULL) {
      ssh_set_error_oom(session);
      session->kbdint->nprompts = i;
      ssh_kbdint_free(session->kbdint);
      session->kbdint = NULL;
      leave_function();
      return SSH_PACKET_USED;
    }
  }
  session->auth_state=SSH_AUTH_STATE_INFO;
  leave_function();
  return SSH_PACKET_USED;
}

/**
 * @brief Try to authenticate through the "keyboard-interactive" method.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @param[in]  user     The username to authenticate. You can specify NULL if
 *                      ssh_option_set_username() has been used. You cannot try
 *                      two different logins in a row.
 *
 * @param[in]  submethods Undocumented. Set it to NULL.
 *
 * @returns SSH_AUTH_ERROR:   A serious error happened\n
 *          SSH_AUTH_DENIED:  Authentication failed : use another method\n
 *          SSH_AUTH_PARTIAL: You've been partially authenticated, you still
 *                            have to use another method\n
 *          SSH_AUTH_SUCCESS: Authentication success\n
 *          SSH_AUTH_INFO:    The server asked some questions. Use
 *                            ssh_userauth_kbdint_getnprompts() and such.\n
 *          SSH_AUTH_AGAIN:   In nonblocking mode, you've got to call this again
 *                            later.
 *
 * @see ssh_userauth_kbdint_getnprompts()
 * @see ssh_userauth_kbdint_getname()
 * @see ssh_userauth_kbdint_getinstruction()
 * @see ssh_userauth_kbdint_getprompt()
 * @see ssh_userauth_kbdint_setanswer()
 */
int ssh_userauth_kbdint(ssh_session session, const char *user,
    const char *submethods) {
    int rc = SSH_AUTH_ERROR;

    if (session == NULL) {
        return SSH_AUTH_ERROR;
    }

#ifdef WITH_SSH1
    if (session->version == 1) {
        return SSH_AUTH_DENIED;
    }
#endif

    if (session->kbdint == NULL) {
        rc = ssh_userauth_kbdint_init(session, user, submethods);

        return rc;
    } else {
        /*
         * If we are at this point, it is because session->kbdint exists.
         * It means the user has set some information there we need to send
         * the server and then we need to ack the status (new questions or ok
         * pass in).
         */
        rc = ssh_userauth_kbdint_send(session);

        return rc;
    }

    return SSH_AUTH_DENIED;
}

/**
 * @brief Get the number of prompts (questions) the server has given.
 *
 * You have called ssh_userauth_kbdint() and got SSH_AUTH_INFO. This
 * function returns the questions from the server.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @returns             The number of prompts.
 */
int ssh_userauth_kbdint_getnprompts(ssh_session session) {
  if(session==NULL)
    return SSH_ERROR;
  if(session->kbdint == NULL) {
    ssh_set_error_invalid(session, __FUNCTION__);
    return SSH_ERROR;
  }
  return session->kbdint->nprompts;
}

/**
 * @brief Get the "name" of the message block.
 *
 * You have called ssh_userauth_kbdint() and got SSH_AUTH_INFO. This
 * function returns the questions from the server.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @returns             The name of the message block. Do not free it.
 */
const char *ssh_userauth_kbdint_getname(ssh_session session) {
  if(session==NULL)
    return NULL;
  if(session->kbdint == NULL) {
    ssh_set_error_invalid(session, __FUNCTION__);
    return NULL;
  }
  return session->kbdint->name;
}

/**
 * @brief Get the "instruction" of the message block.
 *
 * You have called ssh_userauth_kbdint() and got SSH_AUTH_INFO. This
 * function returns the questions from the server.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @returns             The instruction of the message block.
 */

const char *ssh_userauth_kbdint_getinstruction(ssh_session session) {
  if(session==NULL)
    return NULL;
  if(session->kbdint == NULL) {
    ssh_set_error_invalid(session, __FUNCTION__);
    return NULL;
  }
  return session->kbdint->instruction;
}

/**
 * @brief Get a prompt from a message block.
 *
 * You have called ssh_userauth_kbdint() and got SSH_AUTH_INFO. This
 * function returns the questions from the server.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @param[in]  i        The index number of the i'th prompt.
 *
 * @param[in]  echo     When different of NULL, it will obtain a boolean meaning
 *                      that the resulting user input should be echoed or not
 *                      (like passwords).
 *
 * @returns             A pointer to the prompt. Do not free it.
 */
const char *ssh_userauth_kbdint_getprompt(ssh_session session, unsigned int i,
    char *echo) {
  if(session==NULL)
    return NULL;
  if(session->kbdint == NULL) {
    ssh_set_error_invalid(session, __FUNCTION__);
    return NULL;
  }
  if (i > session->kbdint->nprompts) {
    ssh_set_error_invalid(session, __FUNCTION__);
    return NULL;
  }

  if (echo) {
    *echo = session->kbdint->echo[i];
  }

  return session->kbdint->prompts[i];
}

#ifdef WITH_SERVER
/**
 * @brief Get the number of answers the client has given.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @returns             The number of answers.
 */
int ssh_userauth_kbdint_getnanswers(ssh_session session) {
  if(session==NULL || session->kbdint == NULL)
	  return SSH_ERROR;
  return session->kbdint->nanswers;
}

/**
 * @brief Get the answer for a question from a message block.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @param[in]  i index  The number of the ith answer.
 *
 * @return              0 on success, < 0 on error.
 */
const char *ssh_userauth_kbdint_getanswer(ssh_session session, unsigned int i) {
  if(session==NULL || session->kbdint == NULL
                   || session->kbdint->answers == NULL) {
    return NULL;
  }
  if (i > session->kbdint->nanswers) {
    return NULL;
  }

  return session->kbdint->answers[i];
}
#endif

/**
 * @brief Set the answer for a question from a message block.
 *
 * If you have called ssh_userauth_kbdint() and got SSH_AUTH_INFO, this
 * function returns the questions from the server.
 *
 * @param[in]  session  The ssh session to use.
 *
 * @param[in]  i index  The number of the ith prompt.
 *
 * @param[in]  answer   The answer to give to the server.
 *
 * @return              0 on success, < 0 on error.
 */
int ssh_userauth_kbdint_setanswer(ssh_session session, unsigned int i,
    const char *answer) {
  if (session == NULL)
    return -1;
  if (answer == NULL || session->kbdint == NULL ||
      i > session->kbdint->nprompts) {
    ssh_set_error_invalid(session, __FUNCTION__);
    return -1;
  }

  if (session->kbdint->answers == NULL) {
    session->kbdint->answers = malloc(sizeof(char*) * session->kbdint->nprompts);
    if (session->kbdint->answers == NULL) {
      ssh_set_error_oom(session);
      return -1;
    }
    memset(session->kbdint->answers, 0, sizeof(char *) * session->kbdint->nprompts);
  }

  if (session->kbdint->answers[i]) {
    BURN_STRING(session->kbdint->answers[i]);
    SAFE_FREE(session->kbdint->answers[i]);
  }

  session->kbdint->answers[i] = strdup(answer);
  if (session->kbdint->answers[i] == NULL) {
    ssh_set_error_oom(session);
    return -1;
  }

  return 0;
}

/** @} */

/* vim: set ts=4 sw=4 et cindent: */
