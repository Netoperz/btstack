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

#define __BTSTACK_FILE__ "mesh.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble/mesh/adv_bearer.h"
#include "ble/mesh/pb_adv.h"
#include "ble/mesh/beacon.h"
#include "classic/rfcomm.h" // for crc8
#include "btstack.h"
#include "uECC.h"
#include "crypto.h"

static btstack_packet_callback_registration_t hci_event_callback_registration;

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static int counter = 'a';

const uint8_t adv_data[] = {
    // Flags general discoverable, BR/EDR not supported
    0x02, 0x01, 0x06, 
    // Name
    0x0b, 0x09, 'L', 'E', ' ', 'C', 'o', 'u', 'n', 't', 'e', 'r', 
};
const uint8_t adv_data_len = sizeof(adv_data);

const static uint8_t device_uuid[] = { 0x00, 0x1B, 0xDC, 0x08, 0x10, 0x21, 0x0B, 0x0E, 0x0A, 0x0C, 0x00, 0x0B, 0x0E, 0x0A, 0x0C, 0x00 };

// micro-ecc
static uint8_t ec_d[32];
static uint8_t ec_q[64];

// remote ecc
static uint8_t remote_ec_q[64];
static uint8_t dhkey[32];

static void parse_hex(uint8_t * buffer, const char * hex_string){
    while (*hex_string){
        int high_nibble = nibble_for_char(*hex_string++);
        int low_nibble  = nibble_for_char(*hex_string++);
        *buffer++       = (high_nibble << 4) | low_nibble;
    }
}

static void use_fixed_ec_keypair(void){
    const char * ec_d_string =  "3f49f6d4a3c55f3874c9b3e3d2103f504aff607beb40b7995899b8a6cd3c1abd";
    const char * ec_qx_string = "20b003d2f297be2c5e2c83a7e9f9a5b9eff49111acf4fddbcc0301480e359de6";
    const char * ec_qy_string = "dc809c49652aeb6d63329abf5a52155c766345c28fed3024741c8ed01589d28b";
    parse_hex(ec_d, ec_d_string);
    parse_hex(&ec_q[0],  ec_qx_string);
    parse_hex(&ec_q[32], ec_qy_string);
}


static void provisioning_calculate_dhkey(void){
#if uECC_SUPPORTS_secp256r1
    // standard version
    uECC_shared_secret(remote_ec_q, ec_d, dhkey, uECC_secp256r1());
#else
    // static version
    uECC_shared_secret(remote_ec_q, ec_d, dhkey);
#endif
    printf("DHKEY: ");
    printf_hexdump(dhkey, sizeof(dhkey));
}

//

/* LISTING_START(packetHandler): Packet Handler */
static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    bd_addr_t addr;
    int i;

    switch (packet_type) {
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)) {
                case BTSTACK_EVENT_STATE:
                    if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
                    // dump bd_addr in pts format
                    gap_local_bd_addr(addr);
                    printf("Local addr: %s - ", bd_addr_to_str(addr));
                    for (i=0;i<6;i++) {
                        printf("%02x", addr[i]);
                    }
                    printf("\n");
                    // setup scanning
                    gap_set_scan_parameters(0, 0x300, 0x300);
                    gap_start_scan();
                    break;

                default:
                    break;
            }
            break;
    }
}

static uint8_t message[10];
static void generate_message(void){
    memset(message, counter, sizeof(message));
    counter++;
    if (counter >= 'z') {
        counter = 'a';
    }
}

static void mesh_message_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET) return;
    switch(packet[0]){
        case HCI_EVENT_MESH_META:
            switch(packet[2]){
                case MESH_SUBEVENT_CAN_SEND_NOW:
                    generate_message();
                    adv_bearer_send_mesh_message(&message[0], sizeof(message));
                    adv_bearer_request_can_send_now_for_mesh_message();
                    break;
                default:
                    break;
            }
            break;
        case GAP_EVENT_ADVERTISING_REPORT:
            printf("received mesh message\n");
            break;
        default:
            break;
    }
}

static void mesh_unprovisioned_beacon_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t device_uuid[16];
    uint16_t oob;
    const uint8_t * data;
    switch(packet[0]){
        case GAP_EVENT_ADVERTISING_REPORT:
            data = gap_event_advertising_report_get_data(packet);
            memcpy(device_uuid, &packet[3], 16);
            oob = big_endian_read_16(data, 19);
            printf("received unprovisioned device beacon, oob data %x, device uuid: ", oob);
            printf_hexdump(device_uuid, 16);
            break;
        default:
            break;
    }
}

// Provisioning Bearer Control

#define MESH_PROV_INVITE            0x00
#define MESH_PROV_CAPABILITIES      0x01
#define MESH_PROV_START             0x02
#define MESH_PROV_PUB_KEY           0x03
#define MESH_PROV_INPUT_COMPLETE    0x04
#define MESH_PROV_CONFIRM           0x05
#define MESH_PROV_RANDOM            0x06
#define MESH_PROV_DATA              0x07
#define MESH_PROV_COMPLETE          0x08
#define MESH_PROV_FAILED            0x09

#define MESH_OUTPUT_OOB_BLINK       0x01
#define MESH_OUTPUT_OOB_BEEP        0x02
#define MESH_OUTPUT_OOB_VIBRATE     0x04
#define MESH_OUTPUT_OOB_NUMBER      0x08
#define MESH_OUTPUT_OOB_STRING      0x10

#define MESH_INPUT_OOB_PUSH         0x01
#define MESH_INPUT_OOB_TWIST        0x02
#define MESH_INPUT_OOB_NUMBER       0x04
#define MESH_INPUT_OOB_STRING       0x08


#if 0
typedef enum {
    PROV_STATE_W4_INVITE,
    PROV_STATE_W2_SEND_CAPABILITIES,
} prov_state_t;
static prov_state_t prov_state;
#endif

static uint8_t  prov_buffer_out[100];   // TODO: how large are prov messages?
// ConfirmationInputs = ProvisioningInvitePDUValue || ProvisioningCapabilitiesPDUValue || ProvisioningStartPDUValue || PublicKeyProvisioner || PublicKeyDevice
static uint8_t  prov_confirmation_inputs[1 + 11 + 5 + 64 + 64];
static uint8_t  prov_authentication_action;

static btstack_crypto_aes128_cmac_t prov_cmac_request;

static void provisioning_handle_invite(uint8_t *packet, uint16_t size){

    if (size != 1) return;

    // store for confirmation inputs: len 1
    memcpy(&prov_confirmation_inputs[0], packet, 1);

    // handle invite message

    // TODO: store Attention Timer State

    // setup response 
    prov_buffer_out[0] = MESH_PROV_CAPABILITIES;

    // TOOD: get actual number
    /* Number of Elements supported */
    prov_buffer_out[1] = 1;

    /* Supported algorithms - FIPS P-256 Eliptic Curve */
    big_endian_store_16(prov_buffer_out, 2, 1);

    /* Public Key Type - Public Key OOB information available */
    prov_buffer_out[4] = 0;

    /* Static OOB Type - Static OOB information available */
    prov_buffer_out[5] = 1; 

    /* Output OOB Size - max of 8 */
    prov_buffer_out[6] = 8; 

    /* Output OOB Action */
    big_endian_store_16(prov_buffer_out, 7, MESH_OUTPUT_OOB_NUMBER); //  | MESH_OUTPUT_OOB_STRING);

    /* Input OOB Size - max of 8*/
    prov_buffer_out[9] = 8; 

    /* Input OOB Action */
    big_endian_store_16(prov_buffer_out, 10, MESH_INPUT_OOB_STRING | MESH_OUTPUT_OOB_NUMBER);

    // append invite
    memcpy(&prov_confirmation_inputs[1], &prov_buffer_out[1], 11);

    // send
    pb_adv_send_pdu(prov_buffer_out, 12);
}

static void provisioning_handle_start(uint8_t * packet, uint16_t size){

    if (size != 5) return;

    // store for confirmation inputs: len 5
    memcpy(&prov_confirmation_inputs[12], packet, 5);

    // output authentication action
    prov_authentication_action = packet[3];
    printf("Authentication Action: %02x\n", prov_authentication_action);
}

static void provisioning_handle_public_key(uint8_t *packet, uint16_t size){

    if (size != sizeof(remote_ec_q)) return;

    // store for confirmation inputs: len 64
    memcpy(&prov_confirmation_inputs[17], packet, 64);

    // store remote q
    memcpy(remote_ec_q, packet, sizeof(remote_ec_q));

    // calculate DHKey
    provisioning_calculate_dhkey();

    // setup response 
    prov_buffer_out[0] = MESH_PROV_PUB_KEY;
    memcpy(&prov_buffer_out[1], ec_q, 64);

    // append public key
    memcpy(&prov_confirmation_inputs[81], &prov_buffer_out[1], 64);

    // send
    pb_adv_send_pdu(prov_buffer_out, 65);
}

// ConfirmationDevice
static uint8_t confirmation_device[16];

static void provisioning_handle_confirmation_device_calculated(void * arg){
    UNUSED(arg);

    printf("ConfirmationDevice: ");
    printf_hexdump(confirmation_device, sizeof(confirmation_device));

    // setup response 
    prov_buffer_out[0] = MESH_PROV_CONFIRM;
    memcpy(&prov_buffer_out[1], confirmation_device, 16);

    // send
    pb_adv_send_pdu(prov_buffer_out, 17);
}

static void provisioning_handle_confirmation(uint8_t *packet, uint16_t size){

    UNUSED(size);
    UNUSED(packet);

    // CalculationInputs
    uint8_t confirmation_salt[16];
    printf("CalculationInputs: ");
    printf_hexdump(prov_confirmation_inputs, sizeof(prov_confirmation_inputs));
    // ClaculationSalt
    s1(confirmation_salt, prov_confirmation_inputs, sizeof(prov_confirmation_inputs));
    printf("CalculationSalt: ");
    printf_hexdump(confirmation_salt, sizeof(confirmation_salt));
    // ConfirmationKey
    uint8_t confirmation_key[16];
    k1(confirmation_key, dhkey, sizeof(dhkey), confirmation_salt, (uint8_t*) "prck", 4);
    printf("ConfirmationKey: ");
    printf_hexdump(confirmation_key, sizeof(confirmation_key));

    uint8_t auth_value[16];
    memset(auth_value, 0, sizeof(auth_value));
    auth_value[15] = prov_authentication_action;

    // TODO: use random
    memset(&prov_confirmation_inputs[ 0], 0, 16);
    memcpy(&prov_confirmation_inputs[16], auth_value, 16);

    btstack_crypto_aes128_cmac_message(&prov_cmac_request, confirmation_key, 32, prov_confirmation_inputs, confirmation_device, &provisioning_handle_confirmation_device_calculated, NULL);
}

static void provisioning_handle_random(uint8_t *packet, uint16_t size){

    UNUSED(size);
    UNUSED(packet);

    // setup response 
    prov_buffer_out[0] = MESH_PROV_RANDOM;
    memset(&prov_buffer_out[1], 0, 16);

    // send
    pb_adv_send_pdu(prov_buffer_out, 17);
}

static void provisioning_handle_pdu(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){

    if (size < 1) return;

    switch (packet_type){
        case HCI_EVENT_PACKET:
            break;
        case PROVISIONING_DATA_PACKET:
            // dispatch msg
            switch (packet[0]){
                case MESH_PROV_INVITE:
                    printf("MESH_PROV_INVITE\n");
                    printf_hexdump(packet, size);
                    provisioning_handle_invite(&packet[1], size-1);
                    break;
                case MESH_PROV_START:
                    printf("MESH_PROV_START\n");
                    printf_hexdump(&packet[1], size-1);
                    provisioning_handle_start(&packet[1], size-1);
                    break;
                case MESH_PROV_PUB_KEY:
                    printf("MESH_PROV_PUB_KEY\n");
                    printf_hexdump(&packet[1], size-1);
                    provisioning_handle_public_key(&packet[1], size-1);
                    break;
                case MESH_PROV_CONFIRM:
                    printf("MESH_PROV_CONFIRM\n");
                    printf_hexdump(&packet[1], size-1);
                    provisioning_handle_confirmation(&packet[1], size-1);
                    break;
                case MESH_PROV_RANDOM:
                    printf("MESH_PROV_RANDOM\n");
                    printf_hexdump(&packet[1], size-1);
                    provisioning_handle_random(&packet[1], size-1);
                    break;
                default:
                    printf("TODO: handle provisioning msg type %x\n", packet[0]);
                    printf_hexdump(&packet[1], size-1);
                    break;
            }            
            break;
        default:
            break;
    }
}

static void provisioning_init(void){
    pb_adv_init(device_uuid);
    pb_adv_register_packet_handler(&provisioning_handle_pdu);
}

static void stdin_process(char cmd){
    switch (cmd){
        case '1':
            adv_bearer_request_can_send_now_for_mesh_message();
            break;
        default:
            printf("Command: '%c'\n", cmd);
            break;
    }
}

int btstack_main(void);
int btstack_main(void)
{
    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // console
    btstack_stdin_setup(stdin_process);

    // mesh
    adv_bearer_init();
    adv_bearer_register_for_mesh_message(&mesh_message_handler);

    beacon_init(device_uuid, 0);
    beacon_register_for_unprovisioned_device_beacons(&mesh_unprovisioned_beacon_handler);
    
    provisioning_init();

    use_fixed_ec_keypair();

    // turn on!
	hci_power_control(HCI_POWER_ON);
	    
    return 0;
}
/* EXAMPLE_END */
