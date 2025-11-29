#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"

/* NimBLE store (bond/IRK persistence): forward-declare init function */

/* BLE */
#include "console/console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "gatt_svr.h"
#include "hid_func.h"

#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR_REV(a) (a)[5], (a)[4], (a)[3], (a)[2], (a)[1], (a)[0]

static const char *tag = "NimBLEKBD_BLEFUNC";

static int bleprph_gap_event(struct ble_gap_event *event, void *arg);
static uint8_t own_addr_type;

/* Forward decl: provided by NimBLE store module */
void ble_store_config_init(void);

/**
 * Logs information about a connection to the console.
 */
static void
bleprph_print_conn_desc(struct ble_gap_conn_desc *desc)
{
    ESP_LOGI(tag, "handle=%d our_ota_addr_type=%d our_ota_addr=" MACSTR
                " our_id_addr_type=%d our_id_addr=" MACSTR
                " peer_ota_addr_type=%d peer_ota_addr=" MACSTR
                " peer_id_addr_type=%d peer_id_addr=" MACSTR
                " conn_itvl=%d conn_latency=%d supervision_timeout=%d "
                "encrypted=%d authenticated=%d bonded=%d\n",
                desc->conn_handle,
                desc->our_ota_addr.type,
                MAC2STR_REV(desc->our_ota_addr.val),
                desc->our_id_addr.type,
                MAC2STR_REV(desc->our_id_addr.val),
                desc->peer_ota_addr.type,
                MAC2STR_REV(desc->peer_ota_addr.val),
                desc->peer_id_addr.type,
                MAC2STR_REV(desc->peer_id_addr.val),
                desc->conn_itvl, desc->conn_latency,
                desc->supervision_timeout,
                desc->sec_state.encrypted,
                desc->sec_state.authenticated,
                desc->sec_state.bonded);
}

int
user_parse(const struct ble_hs_adv_field *data, void *arg)
{
    ESP_LOGI(tag, "Parse data: field len %d, type %x, total %d bytes",
        data->length, data->type, data->length + 2); /* first byte type and second byte length */
    return 1;
}

/**
 * Enables advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */
static void
bleprph_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name;
    int rc;

    /**
     *  Set the advertisement data included in our advertisements:
     *     o Flags (indicates advertisement type and other general info).
     *     o Advertising tx power.
     *     o Device name.
     *     o 16-bit service UUIDs (alert notifications).
     */

    memset(&fields, 0, sizeof fields);

    /* Advertise two flags:
     *     o Discoverability in forthcoming advertisement (general)
     *     o BLE-only (BR/EDR unsupported).
     */
    fields.flags = BLE_HS_ADV_F_DISC_GEN |
                   BLE_HS_ADV_F_BREDR_UNSUP;

    /* Indicate that the TX power level field should be included; have the
     * stack fill this value automatically.  This is done by assigning the
     * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
     */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.adv_itvl_is_present = 1;
    fields.adv_itvl = 40;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.appearance = HID_KEYBOARD_APPEARENCE;
    fields.appearance_is_present = 1;

    fields.uuids16 = (ble_uuid16_t[]) {
        BLE_UUID16_INIT(GATT_UUID_HID_SERVICE)
    };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    uint8_t buf[50], buf_sz;
    rc = ble_hs_adv_set_fields(&fields, buf, &buf_sz, 50);
    if (rc != 0) {
        ESP_LOGE(tag, "error setting advertisement data to buf; rc=%d", rc);
        return;
    }
    if (buf_sz > BLE_HS_ADV_MAX_SZ) {
        ESP_LOGE(tag, "Too long advertising data: name %s, appearance %x, uuid16 %x, advsize = %d",
            name, fields.appearance, GATT_UUID_HID_SERVICE, buf_sz);
        ble_hs_adv_parse(buf, buf_sz, user_parse, NULL);
        return;
    }

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(tag, "error setting advertisement data; rc=%d", rc);
        return;
    }

    /* Begin advertising. */
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, bleprph_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(tag, "error enabling advertisement; rc=%d", rc);
        return;
    }
}

// default password for bonding, can be changed from sdkconfig var CONFIG_EXAMPLE_DISP_PASSWD
int Disp_password = 123456;

/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that forms.
 * bleprph uses the same callback for all connections.
 *
 * @param event                 The type of event being signalled.
 * @param arg                   Application-specified argument; unused by
 *                                  bleprph.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular GAP event being signalled.
 */
static int
bleprph_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed. */
        ESP_LOGI(tag, "connection %s; status=%d ",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            bleprph_print_conn_desc(&desc);

            hid_clean_vars(&desc);
            
            // Proactively initiate security to start pairing
            ESP_LOGI(tag, "Initiating security (pairing)...");
            rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0) {
                ESP_LOGW(tag, "Failed to initiate security; rc=%d", rc);
            }
        } else {
            /* Connection failed; resume advertising. */
            bleprph_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(tag, "disconnect; reason=%d ", event->disconnect.reason);
        hid_set_disconnected();

        /* Connection terminated; resume advertising. */
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        /* The central requested the connection parameters update. */
        ESP_LOGI(tag, "connection update request");
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The central has updated the connection parameters. */
        ESP_LOGI(tag, "connection updated; status=%d ",
                    event->conn_update.status);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(tag, "advertise complete; reason=%d",
                    event->adv_complete.reason);
        bleprph_advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Encryption has been enabled or disabled for this connection. */
        ESP_LOGI(tag, "encryption change event; status=%d conn_handle=%d",
                    event->enc_change.status,
                    event->enc_change.conn_handle);
        
        if (event->enc_change.status == 0) {
            rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(tag, "Pairing SUCCESS - encrypted=%d authenticated=%d bonded=%d",
                    desc.sec_state.encrypted,
                    desc.sec_state.authenticated,
                    desc.sec_state.bonded);
            }
        } else {
            ESP_LOGE(tag, "Encryption/Pairing FAILED with status=%d", event->enc_change.status);
            // Common BLE_HS error codes:
            // 0x201 (513) = BLE_HS_SM_US_ERR(BLE_SM_ERR_PASSKEY) = Passkey entry failed
            // 0x202 (514) = BLE_HS_SM_US_ERR(BLE_SM_ERR_OOB) = OOB not available  
            // 0x203 (515) = BLE_HS_SM_US_ERR(BLE_SM_ERR_AUTHREQ) = Authentication requirements
            // 0x205 (517) = BLE_HS_SM_US_ERR(BLE_SM_ERR_PAIR_NOT_SUPP) = Pairing not supported
            // 0x208 (520) = BLE_HS_SM_US_ERR(BLE_SM_ERR_CONFIRM_MISMATCH) = Confirm value failed
            // 0x20C (524) = BLE_HS_SM_US_ERR(BLE_SM_ERR_TIMEOUT) = Pairing timeout
            // 0x30D (781) = BLE_HS_SM_US_ERR(BLE_SM_ERR_UNSPECIFIED) = Unspecified reason
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(tag, "subscribe event; conn_handle=%d attr_handle=%04X "
                    "reason=%d prev_notify=%d cur_notify=%d prev_indicate=%d cur_indicate=%d",
                    event->subscribe.conn_handle,
                    event->subscribe.attr_handle,
                    event->subscribe.reason,
                    event->subscribe.prev_notify,
                    event->subscribe.cur_notify,
                    event->subscribe.prev_indicate,
                    event->subscribe.cur_indicate);

        hid_set_notify(event->subscribe.attr_handle,
            event->subscribe.cur_notify,
            event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        ESP_LOGI(tag, "notify event; status=%d conn_handle=%d attr_handle=%04X type=%s",
                    event->notify_tx.status,
                    event->notify_tx.conn_handle,
                    event->notify_tx.attr_handle,
                    event->notify_tx.indication?"indicate":"notify");
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(tag, "mtu update event; conn_handle=%d cid=%d mtu=%d",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* We already have a bond with the peer, but it is attempting to
         * establish a new secure link.  This app sacrifices security for
         * convenience: just throw away the old bond and accept the new link.
         */

        /* Delete the old bond. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        assert(rc == 0);
        ble_store_util_delete_peer(&desc.peer_id_addr);

        /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
         * continue with the pairing operation.
         */
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(tag, "PASSKEY_ACTION_EVENT; action=%d conn_handle=%d", 
                 event->passkey.params.action,
                 event->passkey.conn_handle);
        struct ble_sm_io pkey = {0};

        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = Disp_password;
            ESP_LOGI(tag, "Enter passkey %d on the peer side", pkey.passkey);
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(tag, "ble_sm_inject_io result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            ESP_LOGI(tag, "Numeric comparison: %06lu", (unsigned long)event->passkey.params.numcmp);
            pkey.action = event->passkey.params.action;
            pkey.numcmp_accept = 1;
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(tag, "ble_sm_inject_io numcmp result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NONE) {
            ESP_LOGI(tag, "No passkey action required (Just Works pairing)");
        } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT ||
                  event->passkey.params.action == BLE_SM_IOACT_OOB) {
            ESP_LOGE(tag, "BLE_SM_IOACT_INPUT/OOB bonding not supported!");
            return BLE_HS_SM_US_ERR(BLE_SM_ERR_PASSKEY);
        }
        return 0;

    case BLE_GAP_EVENT_PARING_COMPLETE:
        ESP_LOGI(tag, "pairing complete event; status=%d conn_handle=%d",
                    event->pairing_complete.status,
                    event->pairing_complete.conn_handle);
        
        if (event->pairing_complete.status != 0) {
            ESP_LOGE(tag, "Pairing FAILED with status=%d (0x%X)", 
                     event->pairing_complete.status,
                     event->pairing_complete.status);
            // Common BLE_HS_SM error codes:
            // 0x0201 (513) = Passkey entry failed
            // 0x0202 (514) = OOB not available  
            // 0x0203 (515) = Authentication requirements
            // 0x0205 (517) = Pairing not supported
            // 0x0208 (520) = Confirm value mismatch
            // 0x030D (781) = Unspecified reason
        } else {
            ESP_LOGI(tag, "Pairing SUCCESS!");
            rc = ble_gap_conn_find(event->pairing_complete.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(tag, "After pairing: encrypted=%d authenticated=%d bonded=%d",
                    desc.sec_state.encrypted,
                    desc.sec_state.authenticated,
                    desc.sec_state.bonded);
            }
        }
        return 0;

    default:
        ESP_LOGI(tag, "Unknown GAP event: %d", event->type);
        
        /* Map known events to names per provided IDs */
        switch (event->type) {
        case BLE_GAP_EVENT_IDENTITY_RESOLVED: /* 16 */
            ESP_LOGI(tag, "IDENTITY_RESOLVED");
            break;
        case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE: /* 18 */
            ESP_LOGI(tag, "PHY_UPDATE_COMPLETE");
            break;
        case BLE_GAP_EVENT_DATA_LEN_CHG: /* 34 */
            ESP_LOGI(tag, "DATA_LEN_CHG");
            break;
        case BLE_GAP_EVENT_LINK_ESTAB: /* 38 */
            ESP_LOGI(tag, "LINK_ESTAB");
            break;
        default:
            break;
        }
    }

    return 0;
}

static void
bleprph_on_reset(int reason)
{
    ESP_LOGE(tag, "Resetting state; reason=%d", reason);
}

static void
bleprph_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(tag, "error determining address type; rc=%d", rc);
        return;
    }

    /* Printing ADDR */
    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);

    ESP_LOGI(tag, "Device Address: "MACSTR, MAC2STR_REV(addr_val));

    /* Begin advertising. */
    bleprph_advertise();
}

void
bleprph_host_task(void *param)
{
    ESP_LOGI(tag, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}



void
ble_init()
{
    ESP_LOGI(tag, "ble_init: nimble_port_init starting...");
    int rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(tag, "nimble_port_init failed: %d", rc);
        return;
    }
    ESP_LOGI(tag, "ble_init: nimble_port_init done");
    /* Configure persistent storage for bonds/IRKs */
    ble_store_config_init();
    /* Initialize the NimBLE host configuration. */
    ble_hs_cfg.reset_cb = bleprph_on_reset;
    ble_hs_cfg.sync_cb = bleprph_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // set to BLE_SM_IO_CAP_NO_IO ton bond with no questions
    ble_hs_cfg.sm_io_cap = CONFIG_EXAMPLE_IO_TYPE;

#ifdef CONFIG_EXAMPLE_DISP_PASSWD
#if (CONFIG_EXAMPLE_DISP_PASSWD >99999 && CONFIG_EXAMPLE_DISP_PASSWD < 1000000)
        Disp_password = CONFIG_EXAMPLE_DISP_PASSWD;
#endif
#endif

#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
#endif
#ifdef CONFIG_EXAMPLE_MITM
    ble_hs_cfg.sm_mitm = 1;
#endif
#ifdef CONFIG_EXAMPLE_USE_SC
    ble_hs_cfg.sm_sc = 1;
#else
    ble_hs_cfg.sm_sc = 0;
#endif

#ifdef CONFIG_EXAMPLE_BONDING
    /* Explicitly request encryption and identity keys for both peers */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
#endif

    ESP_LOGI(tag, "ble_init: initializing GATT server...");
    rc = gatt_svr_init();
    if (rc != 0) {
        ESP_LOGE(tag, "gatt_svr_init failed: %d", rc);
        return;
    }
    ESP_LOGI(tag, "ble_init: GATT server initialized");

    // --- Commented out because it is set in sdkconfig.h
    // with CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME
    /* Set the default device name. */
    // rc = ble_svc_gap_device_name_set("ble_kbdhid");
    // assert(rc == 0);

    /* Set GAP appearance */
    ESP_LOGI(tag, "ble_init: setting GAP appearance...");
    rc = ble_svc_gap_device_appearance_set(HID_KEYBOARD_APPEARENCE); /* HID Keyboard*/
    if (rc != 0) {
        ESP_LOGE(tag, "ble_svc_gap_device_appearance_set failed: %d", rc);
        return;
    }

    ESP_LOGI(tag, "ble_init: starting NimBLE host task...");
    nimble_port_freertos_init(bleprph_host_task);
    ESP_LOGI(tag, "ble_init: host task started, init complete");
}
