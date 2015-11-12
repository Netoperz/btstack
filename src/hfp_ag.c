/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */
 
// *****************************************************************************
//
// Minimal setup for HFP Audio Gateway (AG) unit (!! UNDER DEVELOPMENT !!)
//
// *****************************************************************************

#include "btstack-config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <btstack/hci_cmds.h>
#include <btstack/run_loop.h>

#include "hci.h"
#include "btstack_memory.h"
#include "hci_dump.h"
#include "l2cap.h"
#include "sdp_query_rfcomm.h"
#include "sdp.h"
#include "debug.h"
#include "hfp.h"
#include "hfp_ag.h"

static const char default_hfp_ag_service_name[] = "Voice gateway";
static uint16_t hfp_supported_features = HFP_DEFAULT_AG_SUPPORTED_FEATURES;
static uint8_t hfp_codecs_nr = 0;
static uint8_t hfp_codecs[HFP_MAX_NUM_CODECS];

static int  hfp_ag_indicators_nr = 0;
static hfp_ag_indicator_t hfp_ag_indicators[HFP_MAX_NUM_AG_INDICATORS];

static int  hfp_ag_call_hold_services_nr = 0;
static char *hfp_ag_call_hold_services[6];
static hfp_callback_t hfp_callback;

static void packet_handler(void * connection, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

hfp_generic_status_indicator_t * get_hfp_generic_status_indicators();
int get_hfp_generic_status_indicators_nr();
void set_hfp_generic_status_indicators(hfp_generic_status_indicator_t * indicators, int indicator_nr);
void set_hfp_ag_indicators(hfp_ag_indicator_t * indicators, int indicator_nr);
int get_hfp_ag_indicators_nr(hfp_connection_t * context);
hfp_ag_indicator_t * get_hfp_ag_indicators(hfp_connection_t * context);


hfp_ag_indicator_t * get_hfp_ag_indicators(hfp_connection_t * context){
    // TODO: save only value, and value changed in the context?
    if (context->ag_indicators_nr != hfp_ag_indicators_nr){
        context->ag_indicators_nr = hfp_ag_indicators_nr;
        memcpy(context->ag_indicators, hfp_ag_indicators, hfp_ag_indicators_nr * sizeof(hfp_ag_indicator_t));
    }
    return (hfp_ag_indicator_t *)&(context->ag_indicators);
}

hfp_ag_indicator_t * get_ag_indicator_for_name(hfp_connection_t * context, const char * name){
    int i;
    for (i = 0; i < context->ag_indicators_nr; i++){
        if (strcmp(context->ag_indicators[i].name, name) == 0){
            return &context->ag_indicators[i];
        }
    }
    return NULL;
}

void set_hfp_ag_indicators(hfp_ag_indicator_t * indicators, int indicator_nr){
    memcpy(hfp_ag_indicators, indicators, indicator_nr * sizeof(hfp_ag_indicator_t));
    hfp_ag_indicators_nr = indicator_nr;
}

int get_hfp_ag_indicators_nr(hfp_connection_t * context){
    if (context->ag_indicators_nr != hfp_ag_indicators_nr){
        context->ag_indicators_nr = hfp_ag_indicators_nr;
        memcpy(context->ag_indicators, hfp_ag_indicators, hfp_ag_indicators_nr * sizeof(hfp_ag_indicator_t));
    }
    return context->ag_indicators_nr;
}


void hfp_ag_register_packet_handler(hfp_callback_t callback){
    if (callback == NULL){
        log_error("hfp_ag_register_packet_handler called with NULL callback");
        return;
    }
    hfp_callback = callback;
}

static int has_codec_negotiation_feature(hfp_connection_t * connection){
    int hf = get_bit(connection->remote_supported_features, HFP_HFSF_CODEC_NEGOTIATION);
    int ag = get_bit(hfp_supported_features, HFP_AGSF_CODEC_NEGOTIATION);
    return hf && ag;
}

static int has_call_waiting_and_3way_calling_feature(hfp_connection_t * connection){
    int hf = get_bit(connection->remote_supported_features, HFP_HFSF_THREE_WAY_CALLING);
    int ag = get_bit(hfp_supported_features, HFP_AGSF_THREE_WAY_CALLING);
    return hf && ag;
}

static int has_hf_indicators_feature(hfp_connection_t * connection){
    int hf = get_bit(connection->remote_supported_features, HFP_HFSF_HF_INDICATORS);
    int ag = get_bit(hfp_supported_features, HFP_AGSF_HF_INDICATORS);
    return hf && ag;
}

void hfp_ag_create_sdp_record(uint8_t * service, int rfcomm_channel_nr, const char * name, uint8_t ability_to_reject_call, uint16_t supported_features){
    if (!name){
        name = default_hfp_ag_service_name;
    }
    hfp_create_sdp_record(service, SDP_HandsfreeAudioGateway, rfcomm_channel_nr, name, supported_features);
    
    // Network
    de_add_number(service, DE_UINT, DE_SIZE_8, ability_to_reject_call);
    /*
     * 0x01 – Ability to reject a call
     * 0x00 – No ability to reject a call
     */
}

static int hfp_ag_exchange_supported_features_cmd(uint16_t cid){
    char buffer[40];
    sprintf(buffer, "\r\n%s:%d\r\n\r\nOK\r\n", HFP_SUPPORTED_FEATURES, hfp_supported_features);
    return send_str_over_rfcomm(cid, buffer);
}

static int hfp_ag_ok(uint16_t cid){
    char buffer[10];
    sprintf(buffer, "\r\nOK\r\n");
    return send_str_over_rfcomm(cid, buffer);
}

static int hfp_ag_error(uint16_t cid){
    char buffer[10];
    sprintf(buffer, "\r\nERROR\r\n");
    return send_str_over_rfcomm(cid, buffer);
}

static int hfp_ag_report_extended_audio_gateway_error(uint16_t cid, uint8_t error){
    char buffer[20];
    sprintf(buffer, "\r\n%s=%d\r\n", HFP_EXTENDED_AUDIO_GATEWAY_ERROR, error);
    return send_str_over_rfcomm(cid, buffer);
}

static int hfp_ag_retrieve_codec_cmd(uint16_t cid){
    return hfp_ag_ok(cid);
}

static int hfp_ag_indicators_join(char * buffer, int buffer_size, hfp_connection_t * context){
    if (buffer_size < get_hfp_ag_indicators_nr(context) * (1 + sizeof(hfp_ag_indicator_t))) return 0;
    int i;
    int offset = 0;
    for (i = 0; i < get_hfp_ag_indicators_nr(context)-1; i++) {
        offset += snprintf(buffer+offset, buffer_size-offset, "(\"%s\",(%d,%d)),", 
            get_hfp_ag_indicators(context)[i].name, 
            get_hfp_ag_indicators(context)[i].min_range, 
            get_hfp_ag_indicators(context)[i].max_range);
    }
    if ( i < get_hfp_ag_indicators_nr(context)){
        offset += snprintf(buffer+offset, buffer_size-offset, "(\"%s\",(%d,%d))", 
            get_hfp_ag_indicators(context)[i].name, 
            get_hfp_ag_indicators(context)[i].min_range, 
            get_hfp_ag_indicators(context)[i].max_range);
    }
    return offset;
}

static int hfp_hf_indicators_join(char * buffer, int buffer_size){
    if (buffer_size < hfp_ag_indicators_nr * 3) return 0;
    int i;
    int offset = 0;
    for (i = 0; i < get_hfp_generic_status_indicators_nr()-1; i++) {
        offset += snprintf(buffer+offset, buffer_size-offset, "%d,", get_hfp_generic_status_indicators()[i].uuid);
    }
    if (i < get_hfp_generic_status_indicators_nr()){
        offset += snprintf(buffer+offset, buffer_size-offset, "%d,", get_hfp_generic_status_indicators()[i].uuid);
    }
    return offset;
}

static int hfp_hf_indicators_initial_status_join(char * buffer, int buffer_size){
    if (buffer_size < get_hfp_generic_status_indicators_nr() * 3) return 0;
    int i;
    int offset = 0;
    for (i = 0; i < get_hfp_generic_status_indicators_nr(); i++) {
        offset += snprintf(buffer+offset, buffer_size-offset, "\r\n%s:%d,%d\r\n", HFP_GENERIC_STATUS_INDICATOR, get_hfp_generic_status_indicators()[i].uuid, get_hfp_generic_status_indicators()[i].state);
    }
    return offset;
}

static int hfp_ag_indicators_status_join(char * buffer, int buffer_size){
    if (buffer_size < hfp_ag_indicators_nr * 3) return 0;
    int i;
    int offset = 0;
    for (i = 0; i < hfp_ag_indicators_nr-1; i++) {
        offset += snprintf(buffer+offset, buffer_size-offset, "%d,", hfp_ag_indicators[i].status); 
    }
    if (i<hfp_ag_indicators_nr){
        offset += snprintf(buffer+offset, buffer_size-offset, "%d", hfp_ag_indicators[i].status);
    }
    return offset;
}

static int hfp_ag_call_services_join(char * buffer, int buffer_size){
    if (buffer_size < hfp_ag_call_hold_services_nr * 3) return 0;
    int i;
    int offset = snprintf(buffer, buffer_size, "("); 
    for (i = 0; i < hfp_ag_call_hold_services_nr-1; i++) {
        offset += snprintf(buffer+offset, buffer_size-offset, "%s,", hfp_ag_call_hold_services[i]); 
    }
    if (i<hfp_ag_call_hold_services_nr){
        offset += snprintf(buffer+offset, buffer_size-offset, "%s)", hfp_ag_call_hold_services[i]);
    }
    return offset;
}

static int hfp_ag_retrieve_indicators_cmd(uint16_t cid, hfp_connection_t * context){
    char buffer[250];
    int offset = snprintf(buffer, sizeof(buffer), "\r\n%s:", HFP_INDICATOR);
    offset += hfp_ag_indicators_join(buffer+offset, sizeof(buffer)-offset, context);
    
    buffer[offset] = 0;
    
    offset += snprintf(buffer+offset, sizeof(buffer)-offset, "\r\n\r\nOK\r\n");
    buffer[offset] = 0;
    return send_str_over_rfcomm(cid, buffer);
}

static int hfp_ag_retrieve_indicators_status_cmd(uint16_t cid){
    char buffer[40];
    int offset = snprintf(buffer, sizeof(buffer), "\r\n%s:", HFP_INDICATOR);
    offset += hfp_ag_indicators_status_join(buffer+offset, sizeof(buffer)-offset);
    
    buffer[offset] = 0;
    
    offset += snprintf(buffer+offset, sizeof(buffer)-offset, "\r\n\r\nOK\r\n");
    buffer[offset] = 0;
    return send_str_over_rfcomm(cid, buffer);
}

static int hfp_ag_set_indicator_status_update_cmd(uint16_t cid, uint8_t activate){
    // AT\r\n%s:3,0,0,%d\r\n
    return hfp_ag_ok(cid);
}


static int hfp_ag_retrieve_can_hold_call_cmd(uint16_t cid){
    char buffer[100];
    int offset = snprintf(buffer, sizeof(buffer), "\r\n%s:", HFP_SUPPORT_CALL_HOLD_AND_MULTIPARTY_SERVICES);
    offset += hfp_ag_call_services_join(buffer+offset, sizeof(buffer)-offset);
    
    buffer[offset] = 0;
    
    offset += snprintf(buffer+offset, sizeof(buffer)-offset, "\r\n\r\nOK\r\n");
    buffer[offset] = 0;
    return send_str_over_rfcomm(cid, buffer);
}


static int hfp_ag_list_supported_generic_status_indicators_cmd(uint16_t cid){
    return hfp_ag_ok(cid);
}

static int hfp_ag_retrieve_supported_generic_status_indicators_cmd(uint16_t cid){
    char buffer[40];
    int offset = snprintf(buffer, sizeof(buffer), "\r\n%s:", HFP_GENERIC_STATUS_INDICATOR);
    offset += hfp_hf_indicators_join(buffer+offset, sizeof(buffer)-offset);
    
    buffer[offset] = 0;
    
    offset += snprintf(buffer+offset, sizeof(buffer)-offset, "\r\n\r\nOK\r\n");
    buffer[offset] = 0;
    return send_str_over_rfcomm(cid, buffer);
}

static int hfp_ag_retrieve_initital_supported_generic_status_indicators_cmd(uint16_t cid){
    char buffer[40];
    int offset = hfp_hf_indicators_initial_status_join(buffer, sizeof(buffer));
    
    buffer[offset] = 0;
    offset += snprintf(buffer+offset, sizeof(buffer)-offset, "\r\nOK\r\n");
    buffer[offset] = 0;
    return send_str_over_rfcomm(cid, buffer);
}

static int hfp_ag_transfer_ag_indicators_status_cmd(uint16_t cid, hfp_ag_indicator_t * indicator){
    char buffer[20];
    sprintf(buffer, "\r\n%s:%d,%d\r\n", HFP_TRANSFER_AG_INDICATOR_STATUS, indicator->index, indicator->status);
    return send_str_over_rfcomm(cid, buffer);
}

static int hfp_ag_report_network_operator_name_cmd(uint16_t cid, hfp_network_opearator_t op){
    char buffer[40];
    if (strlen(op.name) == 0){
        sprintf(buffer, "\r\n%s:%d,,\r\n\r\nOK\r\n", HFP_QUERY_OPERATOR_SELECTION, op.mode);
    } else {
        sprintf(buffer, "\r\n%s:%d,%d,%s\r\n\r\nOK\r\n", HFP_QUERY_OPERATOR_SELECTION, op.mode, op.format, op.name);
    }
    return send_str_over_rfcomm(cid, buffer);
}


static int hfp_ag_cmd_suggest_codec(uint16_t cid, uint8_t codec){
    char buffer[30];
    sprintf(buffer, "\r\n%s:%d\r\n", HFP_CONFIRM_COMMON_CODEC, codec);
    return send_str_over_rfcomm(cid, buffer);
}

static uint8_t hfp_ag_suggest_codec(hfp_connection_t *context){
    int i,j;
    uint8_t codec = 0;
    for (i = 0; i < hfp_codecs_nr; i++){
        for (j = 0; j < context->remote_codecs_nr; j++){
            if (context->remote_codecs[j] == hfp_codecs[i]){
                codec = context->remote_codecs[j];
                continue;
            }
        }
    }
    return codec;
}


static int hfp_ag_run_for_context_service_level_connection(hfp_connection_t * context){
    if (context->state >= HFP_CODECS_CONNECTION_ESTABLISHED) return 0;
    int done = 0;
    // printf(" AG run for context_service_level_connection 1\n");
    
    switch(context->command){
        case HFP_CMD_SUPPORTED_FEATURES:
            switch(context->state){
                case HFP_W4_EXCHANGE_SUPPORTED_FEATURES:
                    hfp_ag_exchange_supported_features_cmd(context->rfcomm_cid);
                    done = 1;
                    if (has_codec_negotiation_feature(context)){
                        context->state = HFP_W4_NOTIFY_ON_CODECS;
                        break;
                    } 
                    context->state = HFP_W4_RETRIEVE_INDICATORS;
                    break;
                default:
                    break;
            }
            break;
        case HFP_CMD_AVAILABLE_CODECS:
            switch(context->state){
                case HFP_W4_NOTIFY_ON_CODECS:
                    hfp_ag_retrieve_codec_cmd(context->rfcomm_cid);
                    done = 1;
                    context->state = HFP_W4_RETRIEVE_INDICATORS;
                    break;
                case HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED:
                    context->suggested_codec = hfp_ag_suggest_codec(context);
                    //printf("received BAC == new HF codecs, suggested codec %d\n", context->suggested_codec);
                    hfp_ag_ok(context->rfcomm_cid);
                    done = 1;
                    break;

                default:
                    break;
            }
            break;
        case HFP_CMD_INDICATOR:
            switch(context->state){
                case HFP_W4_RETRIEVE_INDICATORS:
                    if (context->retrieve_ag_indicators == 0) break;
                    hfp_ag_retrieve_indicators_cmd(context->rfcomm_cid, context);
                    done = 1;
                    context->state = HFP_W4_RETRIEVE_INDICATORS_STATUS;
                    break;
                case HFP_W4_RETRIEVE_INDICATORS_STATUS:
                    if (context->retrieve_ag_indicators_status == 0) break;
                    hfp_ag_retrieve_indicators_status_cmd(context->rfcomm_cid);
                    done = 1;
                    context->state = HFP_W4_ENABLE_INDICATORS_STATUS_UPDATE;
                    break;
                default:
                    break;
            }
            break;
        case HFP_CMD_ENABLE_INDICATOR_STATUS_UPDATE:
            switch(context->state){
                case HFP_W4_ENABLE_INDICATORS_STATUS_UPDATE:
                    hfp_ag_set_indicator_status_update_cmd(context->rfcomm_cid, 1);
                    done = 1;
                    if (has_call_waiting_and_3way_calling_feature(context)){
                        context->state = HFP_W4_RETRIEVE_CAN_HOLD_CALL;
                        break;
                    }
                    if (has_hf_indicators_feature(context)){
                        context->state = HFP_W4_LIST_GENERIC_STATUS_INDICATORS;
                        break;
                    } 
                    context->state = HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED;
                    hfp_emit_event(hfp_callback, HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED, 0);
                    break;
                default:
                    break;
            }
            break;
        case HFP_CMD_SUPPORT_CALL_HOLD_AND_MULTIPARTY_SERVICES:
            switch(context->state){
                case HFP_W4_RETRIEVE_CAN_HOLD_CALL:
                    hfp_ag_retrieve_can_hold_call_cmd(context->rfcomm_cid);
                    done = 1;
                    if (has_hf_indicators_feature(context)){
                        context->state = HFP_W4_LIST_GENERIC_STATUS_INDICATORS;
                        break;
                    } 
                    context->state = HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED;
                    hfp_emit_event(hfp_callback, HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED, 0);
                    break;
                default:
                    break;
            }
            break;
        case HFP_CMD_GENERIC_STATUS_INDICATOR:
            switch(context->state){
                case HFP_W4_LIST_GENERIC_STATUS_INDICATORS:
                    if (context->list_generic_status_indicators == 0) break;
                    hfp_ag_list_supported_generic_status_indicators_cmd(context->rfcomm_cid);
                    done = 1;
                    context->state = HFP_W4_RETRIEVE_GENERIC_STATUS_INDICATORS;
                    context->list_generic_status_indicators = 0;
                    break;
                case HFP_W4_RETRIEVE_GENERIC_STATUS_INDICATORS:
                    if (context->retrieve_generic_status_indicators == 0) break;
                    hfp_ag_retrieve_supported_generic_status_indicators_cmd(context->rfcomm_cid);
                    done = 1;
                    context->state = HFP_W4_RETRIEVE_INITITAL_STATE_GENERIC_STATUS_INDICATORS; 
                    context->retrieve_generic_status_indicators = 0;
                    break;
                case HFP_W4_RETRIEVE_INITITAL_STATE_GENERIC_STATUS_INDICATORS:
                    if (context->retrieve_generic_status_indicators_state == 0) break;
                    hfp_ag_retrieve_initital_supported_generic_status_indicators_cmd(context->rfcomm_cid);
                    done = 1;
                    context->state = HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED;
                    context->retrieve_generic_status_indicators_state = 0;
                    hfp_emit_event(hfp_callback, HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED, 0);
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }
    return done;
}

static int hfp_ag_run_for_context_service_level_connection_queries(hfp_connection_t * context){
    if (context->state != HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED) return 0;
    int done = 0;
    printf("    SLC queries: \n");

    switch(context->command){
        case HFP_CMD_AVAILABLE_CODECS:
            context->suggested_codec = hfp_ag_suggest_codec(context);
            //printf("received BAC == new HF codecs, suggested codec %d\n", context->suggested_codec);
            hfp_ag_ok(context->rfcomm_cid);
            done = 1;
            break;

        case HFP_CMD_QUERY_OPERATOR_SELECTION:
            if (context->operator_name_format == 1){
                if (context->network_operator.format != 0){
                    hfp_ag_error(context->rfcomm_cid);
                    done = 1;
                    break;
                }
                hfp_ag_ok(context->rfcomm_cid);
                done = 1;
                context->operator_name_format = 0;    
                break;
            }
            if (context->operator_name == 1){
                hfp_ag_report_network_operator_name_cmd(context->rfcomm_cid, context->network_operator);
                context->operator_name = 0;
                done = 1;
                break;
            }
            break;
        case HFP_CMD_ENABLE_INDIVIDUAL_AG_INDICATOR_STATUS_UPDATE:{
                hfp_ag_ok(context->rfcomm_cid);
                done = 1;
                break;
            }
        case HFP_CMD_TRIGGER_CODEC_CONNECTION_SETUP:
            if (context->hf_trigger_codec_connection_setup){ // received BCC
                //printf(" received BCC \n");
                context->hf_trigger_codec_connection_setup = 0;
                context->ag_trigger_codec_connection_setup = 1;
                context->state = HFP_SLE_W2_EXCHANGE_COMMON_CODEC;
                hfp_ag_ok(context->rfcomm_cid);
                done = 1;
                return done;
            }
            
            log_info("SLC queries: ag_trigger_codec_connection_setup");
            if (context->ag_trigger_codec_connection_setup){ // received BCS
                log_info(" send BCS \n");
                context->ag_trigger_codec_connection_setup = 0;
                context->state = HFP_SLE_W4_EXCHANGE_COMMON_CODEC;
                context->suggested_codec = hfp_ag_suggest_codec(context);
                hfp_ag_cmd_suggest_codec(context->rfcomm_cid, context->suggested_codec);
                done = 1;
                return done;
            }
            break;
        case HFP_CMD_ENABLE_EXTENDED_AUDIO_GATEWAY_ERROR:
            if (context->extended_audio_gateway_error){
                hfp_ag_report_extended_audio_gateway_error(context->rfcomm_cid, context->extended_audio_gateway_error);
                context->extended_audio_gateway_error = 0;
                done = 1;
                break;
            }
        case HFP_CMD_ENABLE_INDICATOR_STATUS_UPDATE:
            printf("TODO\n");
            break;
        case HFP_CMD_NONE:
            if (context->start_call){
                context->start_call = 0;
                hfp_ag_indicator_t * indicator = get_ag_indicator_for_name(context, "callsetup");
                if (!indicator) break;
                // TODO: do we need this check?
                if (indicator->status != HFP_CALLSETUP_STATUS_NO_CALL_SETUP_IN_PROGRESS){
                    context->start_call = 1;
                    break;
                }    
                
                indicator->status = HFP_CALLSETUP_STATUS_INCOMING_CALL_SETUP_IN_PROGRESS;
                hfp_ag_transfer_ag_indicators_status_cmd(context->rfcomm_cid, indicator);
                context->state = HFP_SLE_W2_EXCHANGE_COMMON_CODEC;
                context->ag_trigger_codec_connection_setup = 1;
                context->establish_audio_connection = 1;
                context->start_ringing = 1;
                done = 1;
                return done;
            }
        default:
            break;
    }
    return done;
}


static int hfp_ag_run_for_context_codecs_connection(hfp_connection_t * context){
    printf(" AG run for context_codecs_connection: \n");
    if (context->state <= HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED ||
        context->state > HFP_W2_DISCONNECT_SCO) return 0;

    int done = 0;
    
    switch (context->state){
        case HFP_SLE_W2_EXCHANGE_COMMON_CODEC:
            if (context->ag_trigger_codec_connection_setup){ // received BCS
                log_info(" send BCS \n");
                context->ag_trigger_codec_connection_setup = 0;
                context->state = HFP_SLE_W4_EXCHANGE_COMMON_CODEC;
                context->suggested_codec = hfp_ag_suggest_codec(context);
                hfp_ag_cmd_suggest_codec(context->rfcomm_cid, context->suggested_codec);
                done = 1;
                break;
            }
            break;
        case HFP_SLE_W4_EXCHANGE_COMMON_CODEC:
            log_info("entered HFP_SLE_W4_EXCHANGE_COMMON_CODEC state");
            switch(context->command){
                case HFP_CMD_AVAILABLE_CODECS:
                    if (context->notify_ag_on_new_codecs){ // received BAC
                        log_info(" received BAC\n");
                        context->notify_ag_on_new_codecs = 0;
                        if (context->suggested_codec != hfp_ag_suggest_codec(context)){
                            context->suggested_codec = hfp_ag_suggest_codec(context);
                            context->state = HFP_SLE_W2_EXCHANGE_COMMON_CODEC;
                            context->ag_trigger_codec_connection_setup = 1;
                        }
                        hfp_ag_ok(context->rfcomm_cid);
                        done = 1;
                        break;
                    }
                    break;
                case HFP_CMD_HF_CONFIRMED_CODEC:
                    log_info(" received AT+BCS\n");
                    if (context->codec_confirmed != context->suggested_codec){
                        context->state = HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED;
                        hfp_ag_error(context->rfcomm_cid);
                        done = 1;
                        break;
                    } 
                    context->negotiated_codec = context->codec_confirmed;
                    context->state = HFP_CODECS_CONNECTION_ESTABLISHED;
                    hfp_emit_event(hfp_callback, HFP_SUBEVENT_CODECS_CONNECTION_COMPLETE, 0);
                    hfp_ag_ok(context->rfcomm_cid);
                    done = 1;
                    break; 
                default:
                    log_info("command not handled");
                    break;
            }
            break;
            
        case HFP_CODECS_CONNECTION_ESTABLISHED:
            switch(context->command){
                case HFP_CMD_AVAILABLE_CODECS:

                    if (context->notify_ag_on_new_codecs){ // received BAC
                        context->notify_ag_on_new_codecs = 0;
                        if (context->suggested_codec != hfp_ag_suggest_codec(context)){
                            context->suggested_codec = hfp_ag_suggest_codec(context);
                            context->state = HFP_SLE_W4_EXCHANGE_COMMON_CODEC;
                        }
                        hfp_ag_ok(context->rfcomm_cid);
                        done = 1;
                        break;
                    }
                    break;
                case HFP_CMD_AG_SUGGESTED_CODEC:
                    if (context->ag_trigger_codec_connection_setup){ 
                        context->ag_trigger_codec_connection_setup = 0;
                        if (context->negotiated_codec != hfp_ag_suggest_codec(context)){
                            context->state = HFP_SLE_W4_EXCHANGE_COMMON_CODEC;
                            context->suggested_codec = hfp_ag_suggest_codec(context);
                            hfp_ag_cmd_suggest_codec(context->rfcomm_cid, context->suggested_codec);
                            done = 1;
                            break;
                        }
                    }
                    break;
                default:
                    break;
            }
            break;

        case HFP_W2_DISCONNECT_SCO:
            context->state = HFP_W4_SCO_DISCONNECTED;
            gap_disconnect(context->sco_handle);
            done = 1;
            return done;

        case HFP_AUDIO_CONNECTION_ESTABLISHED:
            if (context->release_audio_connection){
                context->state = HFP_W4_SCO_DISCONNECTED;
                gap_disconnect(context->sco_handle);
                done = 1;
                return done;
            }
            if (context->start_ringing){
                context->start_ringing = 0;
                context->state = HFP_RING_ALERT;
                hfp_emit_event(hfp_callback, HFP_SUBEVENT_START_RINGINIG, 0);
            }
            break;
        
        case HFP_RING_ALERT:
            // check if ATA
            if (context->command == HFP_CMD_CALL_ANSWERED){
                context->state = HFP_CALL_ACTIVE;
                context->update_call_status = 1;
                hfp_emit_event(hfp_callback, HFP_SUBEVENT_STOP_RINGINIG, 0);
                hfp_ag_ok(context->rfcomm_cid);
                done = 1;
                return done;
            }
            break;
        case HFP_CALL_ACTIVE:
            if (context->update_call_status){
                context->update_call_status = 0;
                context->update_callsetup_status = 1;
                hfp_ag_indicator_t * indicator = get_ag_indicator_for_name(context, "call");
                indicator->status = HFP_CALL_STATUS_ACTIVE_OR_HELD_CALL_IS_PRESENT;
                hfp_ag_transfer_ag_indicators_status_cmd(context->rfcomm_cid, indicator);
                done = 1;
                return done;
            }
            if (context->update_callsetup_status){
                context->update_callsetup_status = 0;
                hfp_ag_indicator_t * indicator = get_ag_indicator_for_name(context, "callsetup");
                indicator->status = HFP_HELDCALL_STATUS_NO_CALLS_HELD;
                hfp_ag_transfer_ag_indicators_status_cmd(context->rfcomm_cid, indicator);
                done = 1;
                return done;   
            }
            break;
        default:
            break;
    }

    if (done) return done;
    
    if (context->establish_audio_connection){
        if (context->state < HFP_SLE_W4_EXCHANGE_COMMON_CODEC){
            context->ag_trigger_codec_connection_setup = 0;
            context->state = HFP_SLE_W4_EXCHANGE_COMMON_CODEC;
            context->suggested_codec = hfp_ag_suggest_codec(context);
            hfp_ag_cmd_suggest_codec(context->rfcomm_cid, context->suggested_codec);
            done = 1;
            return done;
        } else {
            context->state = HFP_W4_SCO_CONNECTED;
            hci_send_cmd(&hci_setup_synchronous_connection, context->con_handle, 8000, 8000, 0xFFFF, hci_get_sco_voice_setting(), 0xFF, 0x003F);
            done = 1;
            return done;
        }
    }
    
    if (context->release_audio_connection){
        context->state = HFP_W4_SCO_DISCONNECTED;
        gap_disconnect(context->sco_handle);
        done = 1;
        return done;
    }
    
    return done;
}

static void hfp_run_for_context(hfp_connection_t *context){
    log_info("ag hfp_run_for_context entered");

    if (!context) return;
    log_info("ag hfp_run_for_context context found");

    if (!rfcomm_can_send_packet_now(context->rfcomm_cid)) return;
    log_info("ag hfp_run_for_context rfcomm_can_send_packet_now");
    
    if (context->command == HFP_CMD_UNKNOWN){
        log_info("ag hfp_run_for_context HFP_CMD_UNKNOWN");
        hfp_ag_error(context->rfcomm_cid);
        context->send_ok = 0;
        context->send_error = 0;
        context->command = HFP_CMD_NONE;
        return;
    }

    
    if (context->send_ok){
        log_info("ag hfp_run_for_context hfp_ag_ok");
        hfp_ag_ok(context->rfcomm_cid);
        context->send_ok = 0;
        context->command = HFP_CMD_NONE;
        return;
    }

    if (context->send_error){
        log_info("ag hfp_run_for_context hfp_ag_error");
        hfp_ag_error(context->rfcomm_cid); 
        context->send_error = 0;
        context->command = HFP_CMD_NONE;
        return;
    }

    int done = hfp_ag_run_for_context_service_level_connection(context);
    log_info("hfp_ag_run_for_context_service_level_connection = %d", done);

    if (rfcomm_can_send_packet_now(context->rfcomm_cid) && !done){
        done = hfp_ag_run_for_context_service_level_connection_queries(context);
        log_info("hfp_ag_run_for_context_service_level_connection_queries = %d", done);
        
        if (rfcomm_can_send_packet_now(context->rfcomm_cid) && !done){
            done = hfp_ag_run_for_context_codecs_connection(context);
            log_info("hfp_ag_run_for_context_codecs_connection = %d", done);
        }
    }

    if (context->command == HFP_CMD_NONE && !done){
        log_info("context->command == HFP_CMD_NONE");
        switch(context->state){
            case HFP_W2_DISCONNECT_RFCOMM:
                context->state = HFP_W4_RFCOMM_DISCONNECTED;
                rfcomm_disconnect_internal(context->rfcomm_cid);
                break;
            default:
                break;
        }
    }
    if (done){
        context->command = HFP_CMD_NONE;
    }
}

static void hfp_handle_rfcomm_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    hfp_connection_t * context = get_hfp_connection_context_for_rfcomm_cid(channel);
    if (!context) return;
    
    if (context->state == HFP_EXCHANGE_SUPPORTED_FEATURES){
        context->state = HFP_W4_EXCHANGE_SUPPORTED_FEATURES;   
    }

    packet[size] = 0;
    int pos;
    for (pos = 0; pos < size ; pos++){
        hfp_parse(context, packet[pos]);

        // trigger next action after CMD received
        if (context->command == HFP_CMD_NONE) continue;
        //hfp_run_for_context(context);
    }
}

static void hfp_run(){
    linked_list_iterator_t it;    
    linked_list_iterator_init(&it, hfp_get_connections());
    while (linked_list_iterator_has_next(&it)){
        hfp_connection_t * connection = (hfp_connection_t *)linked_list_iterator_next(&it);
        hfp_run_for_context(connection);
    }
}

static void packet_handler(void * connection, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    switch (packet_type){
        case RFCOMM_DATA_PACKET:
            hfp_handle_rfcomm_event(packet_type, channel, packet, size);
            break;
        case HCI_EVENT_PACKET:
            hfp_handle_hci_event(hfp_callback, packet_type, packet, size);
            return;
        default:
            break;
    }

    hfp_run();
}

void hfp_ag_init(uint16_t rfcomm_channel_nr, uint32_t supported_features, 
    uint8_t * codecs, int codecs_nr, 
    hfp_ag_indicator_t * ag_indicators, int ag_indicators_nr,
    hfp_generic_status_indicator_t * hf_indicators, int hf_indicators_nr,
    const char *call_hold_services[], int call_hold_services_nr){
    if (codecs_nr > HFP_MAX_NUM_CODECS){
        log_error("hfp_init: codecs_nr (%d) > HFP_MAX_NUM_CODECS (%d)", codecs_nr, HFP_MAX_NUM_CODECS);
        return;
    }
    l2cap_init();
    l2cap_register_packet_handler(packet_handler);

    rfcomm_register_packet_handler(packet_handler);

    hfp_init(rfcomm_channel_nr);
    
    hfp_supported_features = supported_features;
    hfp_codecs_nr = codecs_nr;

    int i;
    for (i=0; i<codecs_nr; i++){
        hfp_codecs[i] = codecs[i];
    }

    hfp_ag_indicators_nr = ag_indicators_nr;
    memcpy(hfp_ag_indicators, ag_indicators, ag_indicators_nr * sizeof(hfp_ag_indicator_t));
    for (i=0; i<hfp_ag_indicators_nr; i++){
        printf("ag ind %s\n", hfp_ag_indicators[i].name);
    }

    set_hfp_generic_status_indicators(hf_indicators, hf_indicators_nr);

    hfp_ag_call_hold_services_nr = call_hold_services_nr;
    memcpy(hfp_ag_call_hold_services, call_hold_services, call_hold_services_nr * sizeof(char *));
}

void hfp_ag_establish_service_level_connection(bd_addr_t bd_addr){
    hfp_establish_service_level_connection(bd_addr, SDP_Handsfree);
}

void hfp_ag_release_service_level_connection(bd_addr_t bd_addr){
    hfp_connection_t * connection = get_hfp_connection_context_for_bd_addr(bd_addr);
    hfp_release_service_level_connection(connection);
    hfp_run_for_context(connection);
}

void hfp_ag_report_extended_audio_gateway_error_result_code(bd_addr_t bd_addr, hfp_cme_error_t error){
    hfp_connection_t * connection = get_hfp_connection_context_for_bd_addr(bd_addr);
    if (!connection){
        log_error("HFP HF: connection doesn't exist.");
        return;
    }
    connection->extended_audio_gateway_error = 0;
    if (!connection->enable_extended_audio_gateway_error_report){
        return;
    }
    connection->extended_audio_gateway_error = error;
    hfp_run_for_context(connection);
}


void hfp_ag_establish_audio_connection(bd_addr_t bd_addr){
    hfp_ag_establish_service_level_connection(bd_addr);
    hfp_connection_t * connection = get_hfp_connection_context_for_bd_addr(bd_addr);
    if (!has_codec_negotiation_feature(connection)){
        log_info("hfp_ag_establish_audio_connection - no codec negotiation feature");
        return;
    } 
    
    connection->establish_audio_connection = 0;
    if (connection->state == HFP_AUDIO_CONNECTION_ESTABLISHED) return;
    if (connection->state >= HFP_W2_DISCONNECT_SCO) return;
    
    log_info("hfp_ag_establish_audio_connection");
        
    connection->establish_audio_connection = 1;

    if (connection->state < HFP_SLE_W4_EXCHANGE_COMMON_CODEC){
        log_info("hfp_ag_establish_audio_connection ag_trigger_codec_connection_setup");
        connection->command = HFP_CMD_TRIGGER_CODEC_CONNECTION_SETUP;
        connection->ag_trigger_codec_connection_setup = 1;
    }
    hfp_run_for_context(connection);
}

void hfp_ag_release_audio_connection(bd_addr_t bd_addr){
    hfp_connection_t * connection = get_hfp_connection_context_for_bd_addr(bd_addr);
    hfp_release_audio_connection(connection);
    hfp_run_for_context(connection);
}

/**
 * @brief 
 */
void hfp_ag_call(bd_addr_t bd_addr){
    hfp_ag_establish_service_level_connection(bd_addr);
    hfp_connection_t * connection = get_hfp_connection_context_for_bd_addr(bd_addr);
    connection->start_call = 1;
    printf("hfp_ag_call\n");
    hfp_run_for_context(connection);
}


/**
 * @brief 
 */
void hfp_ag_terminate_call(bd_addr_t bd_addr){
    hfp_connection_t * connection = get_hfp_connection_context_for_bd_addr(bd_addr);
    if (connection->state != HFP_AUDIO_CONNECTION_ESTABLISHED) return;
    connection->terminate_call = 1;
    hfp_run_for_context(connection);
}


