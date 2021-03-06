#include "gattc.h"
#include "switching.h"
#include "glue.h"

static const char *const GATTC_TAG = "GATT_CLIENT";
int len_servers;

esp_bd_addr_t bda[MAX_NUM_SERVERS] =
{
	{ 0x30, 0xae, 0xa4, 0x3c, 0x3d, 0xf2 },
	{ 0x30, 0xae, 0xa4, 0x3c, 0x89, 0xf6 },
};

uint8_t rms[MAX_NUM_SERVERS];

bool is_server_equal(const esp_bd_addr_t l, const esp_bd_addr_t r)
{
	return memcmp(l, r, sizeof(esp_bd_addr_t)) == 0;
}

void add_server(const esp_bd_addr_t addr)
{
	for (int i = 0; i < len_servers; i++)
	{
		if (is_server_equal(bda[i], addr))
			return;
	}

	memcpy(bda[len_servers], addr, sizeof(esp_bd_addr_t));
	++len_servers;
}

/* One gatt-based profile one app_id and one gattc_if, this array will store the gattc_if returned by ESP_GATTS_REG_EVT */
gattc_profile_inst profile =
{
	.gattc_cb = gattc_profile_event_handler,
    .gattc_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
};

void gattc_profile_event_handler(
	esp_gattc_cb_event_t event,
	esp_gatt_if_t gattc_if,
	esp_ble_gattc_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GATTC_REG_EVT:
    {
        ESP_LOGI(GATTC_TAG, "REG_EVT");
        esp_ble_gap_start_scanning(5);
        break;
    }

    case ESP_GATTC_CONNECT_EVT:
    {
        ESP_LOGI(
        	GATTC_TAG,
			"ESP_GATTC_CONNECT_EVT conn_id %d, if %d",
			param->connect.conn_id,
			gattc_if);
        profile.conn_id = param->connect.conn_id;
        ESP_LOGI(
        	GATTC_TAG,
			"REMOTE BDA:");
        esp_log_buffer_hex(
        	GATTC_TAG,
			param->connect.remote_bda,
			sizeof(esp_bd_addr_t));
        esp_err_t ret;
        if ((ret = esp_ble_gattc_send_mtu_req(gattc_if, param->connect.conn_id)) != ESP_OK)
        {
            ESP_LOGE(
            	GATTC_TAG,
				"config MTU error, error code = %x",
				ret);
        }
        esp_ble_gattc_register_for_notify(
        	gattc_if,
			param->connect.remote_bda,
			0x2a);
        glue_notify_ble_connected();
        break;
    }

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK)
        {
            ESP_LOGE(
            	GATTC_TAG,
				"open failed, status %d",
				param->open.status);
            break;
        }
        ESP_LOGI(GATTC_TAG, "open success");
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status != ESP_GATT_OK)
        {
            ESP_LOGE(
            	GATTC_TAG,
				"config mtu failed, error status = %x",
				param->cfg_mtu.status);
        }
        ESP_LOGI(
        	GATTC_TAG,
			"ESP_GATTC_CFG_MTU_EVT, Status %d, MTU %d, conn_id %d",
			param->cfg_mtu.status,
			param->cfg_mtu.mtu,
			param->cfg_mtu.conn_id);
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
    	ESP_LOGI(GATTC_TAG, "register for notify");
    	break;

    case ESP_GATTC_NOTIFY_EVT:
    	ESP_LOGI(
    		GATTC_TAG,
			"notified from boi %d: %d",
			param->notify.conn_id,
			param->notify.value[0]);
    	rms[param->notify.conn_id] = param->notify.value[0];
    	handle_rms_notification();
    	break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(
        	GATTC_TAG,
			"ESP_GATTC_DISCONNECT_EVT, reason = %d",
			param->disconnect.reason);
        glue_notify_ble_disconnected();
        break;

    default:
        break;
    }
}

void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
    	if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT)
    	{
			esp_log_buffer_hex(
				GATTC_TAG,
				param->scan_rst.bda,
				6);

			ESP_LOGI(
				GATTC_TAG,
				"searched Adv Data Len %d, Scan Response Len %d",
				param->scan_rst.adv_data_len,
				param->scan_rst.scan_rsp_len);

			uint8_t adv_name_len;
			const char *adv_name = (const char *)esp_ble_resolve_adv_data(
				param->scan_rst.ble_adv,
				ESP_BLE_AD_TYPE_NAME_CMPL,
				&adv_name_len);

			if (adv_name != NULL)
			{
				static const char *const DEVICE_NAME = "SERVER";
				if (strncmp(adv_name, DEVICE_NAME, strlen(DEVICE_NAME)) == 0)
				{
					ESP_LOGI(GATTC_TAG, "Adding a server on idx %d", len_servers);
					add_server(param->scan_rst.bda);
					if (len_servers == MAX_NUM_SERVERS)
						esp_ble_gap_stop_scanning();
				}
			}
			break;
		}

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        for (size_t i = 0; i < MAX_NUM_SERVERS; i++)
        {
        	esp_err_t ret = esp_ble_gattc_open(
        		profile.gattc_if,
        		bda[i],
        		BLE_ADDR_TYPE_PUBLIC,
        		true);

        	if (ret != ESP_OK)
        	{
        		ESP_LOGI(GATTC_TAG, "Cannot connect to %x with: %x", bda[i][5], ret);
        	    break;
        	}

        	vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    	break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
         ESP_LOGI(
        	GATTC_TAG,
			"Connection updated\n"\
				"\tstatus = %d,\n"\
				"\tmin_int = %d,\n"\
				"\tmax_int = %d,\n"\
				"\tconn_int = %d,\n"\
				"\tlatency = %d,\n"\
				"\ttimeout = %d",
            param->update_conn_params.status,
            param->update_conn_params.min_int,
            param->update_conn_params.max_int,
            param->update_conn_params.conn_int,
            param->update_conn_params.latency,
            param->update_conn_params.timeout);
        break;

    default:
        break;
    }
}

void esp_gattc_cb(
	esp_gattc_cb_event_t event,
	esp_gatt_if_t gattc_if,
	esp_ble_gattc_cb_param_t *param)
{
    /* If event is register event, store the gattc_if for each profile */
    if (event == ESP_GATTC_REG_EVT)
    {
    	if (param->reg.status == ESP_GATT_OK)
    	{
    		profile.gattc_if = gattc_if;
    	}
        else
    	{
    		ESP_LOGI(GATTC_TAG,
    			"reg app failed, app_id %04x, status %d",
    			param->reg.app_id,
    			param->reg.status);
    		return;
    	}
    }

	const bool is_none = gattc_if == ESP_GATT_IF_NONE;
	const bool is_this = gattc_if == profile.gattc_if;

	/* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
	if ((is_none || is_this) && profile.gattc_cb)
			profile.gattc_cb(event, gattc_if, param);
}
