/*****************************************************************/ /**
* @file lbs_demo.c
* @brief LBS (Location Based Service) demonstration program
* @author Lambert.Zhao@quectel.com
* @date 2025-10-16
*
* @copyright Copyright (c) 2023 Quectel Wireless Solution, Co., Ltd.
* All Rights Reserved. Quectel Wireless Solution Proprietary and Confidential.
*
* @par EDIT HISTORY FOR MODULE
* <table>
* <tr><th>Date <th>Version <th>Author <th>Description
* <tr><td>2025-10-16 <td>1.0 <td>Lambert.Zhao <td> Initial version
* </table>
**********************************************************************/

#include "qosa_log.h"
#include "qosa_def.h"
#include "qosa_sys.h"
#include "qosa_dev.h"
#include "qosa_network.h"
#include "qcm_lbs_app.h"
#include "stdlib.h"
#include "qosa_system_utils.h"
#include "qosa_datacall.h"
#include "unirtos_app_init_registry.h"

#define QOS_LOG_TAG                             LOG_TAG_LBS_API
#define CONFIG_UNIRTOS_LBS_DEMO_TASK_STACK_SIZE 4096
#define UNIR_LBS_DEMO_TASK_PRIO                 QOSA_PRIORITY_NORMAL
#define LBS_DEMO_USER_NAME                      "unirtos"
#define LBS_DEMO_USER_PWD                       "123456"
#define LBS_DEMO_TOKEN                          "1111111122222222"
#define LBS_DEMO_IMEI                           "861687000001091"

/** LBS demonstration task handle */
static qosa_task_t g_lbs_task = QOSA_NULL;

/** LBS client ID */
static qcm_lbs_client_id g_lbs_cli = 0;

/** LBS semaphore, used to synchronize location request results */
static qosa_sem_t g_lbs_semp;

/** LBS basic information configuration */
static qcm_lbs_basic_info_t g_basic_info
    = {.type = 1,        /*!< Location type */
       .encrypt = 1,     /*!< Encryption flag */
       .key_index = 1,   /*!< Key index */
       .pos_format = 1,  /*!< Position format */
       .loc_method = 4}; /*!< Location method */

/** LBS authentication information configuration */
static qcm_lbs_auth_info_t g_auth_info
    = {.user_name = LBS_DEMO_USER_NAME, /*!< Username */
       .user_pwd = LBS_DEMO_USER_PWD,   /*!< User password */
       .token = LBS_DEMO_TOKEN,         /*!< Authentication token */
       .imei = LBS_DEMO_IMEI,           /*!< Device IMEI */
       .rand = 2346};                       /*!< Random number */

/** LBS cell information configuration */
static qcm_lbs_cell_info_t g_lbs_cell_info[]
    = {{.radio = 3,           /*!< Radio access technology type */
        .mcc = 460,           /*!< Mobile country code */
        .mnc = 0,             /*!< Mobile network code */
        .lac_id = 0x550B,     /*!< Location area code */
        .cell_id = 0xF2D4A48, /*!< Cell ID */
        .signal = 0,          /*!< Signal strength */
        .tac = 3,             /*!< Tracking area code */
        .bcch = 0,            /*!< Broadcast control channel */
        .bsic = 0,            /*!< Base station identity code */
        .uarfcndl = 0,        /*!< UTRA absolute radio frequency channel number downlink */
        .psc = 0,             /*!< Primary scrambling code */
        .rsrq = 0,            /*!< Reference signal received quality */
        .pci = 0,             /*!< Physical cell ID */
        .earfcn = 0}};        /*!< E-UTRA absolute radio frequency channel number */

/**
 * @brief LBS Position result callback function
 *
 * @param[in] client_id
 *          - LBS client ID
 * @param[in] result
 *          - Location result status code
 * @param[in] pos_num
 *          - Number of location information
 * @param[in] pos_info
 *          - Pointer to location information array
 * @param[in] date
 *          - Date time string
 * @param[in] arg
 *          - User parameter
 *
 * @note This function is called after LBS positioning is completed, responsible for processing and outputting location information
 */
static void
unir_lbs_result_cb(qcm_lbs_client_id client_id, qcm_lbs_result_code_e result, qosa_int32_t pos_num, qcm_lbs_postion_info_t *pos_info, char *date, void *arg)
{
    int  i = 0;
    char longitude_str[30] = {0};
    char latitude_str[30] = {0};

    // Check if client ID matches
    if (client_id != g_lbs_cli)
    {
        qosa_free(date);
        return;
    }

    // Check if positioning result is successful and has location information
    if ((QCM_LBS_SUCCESS == result) && (pos_num > 0))
    {
        // Check if location information pointer is valid
        if (QOSA_NULL != pos_info)
        {
            // Traverse all location information and output
            for (i = 0; i < pos_num; i++)
            {
                // Convert longitude and latitude to string format
                qosa_utils_double_to_str_with_precision(pos_info[i].longitude, (qosa_uint8_t *)longitude_str, sizeof(latitude_str) - 1, 6);
                qosa_utils_double_to_str_with_precision(pos_info[i].latitude, (qosa_uint8_t *)latitude_str, sizeof(longitude_str) - 1, 6);
                // Output location information: longitude, latitude, accuracy
                QLOGV("Location[%d]: %s, %s, %d\n", i, longitude_str, latitude_str, pos_info[i].accuracy);
            }
        }
    }
    // Release date string memory
    qosa_free(date);
    // Release semaphore, notify main task that positioning is complete
    qosa_sem_release(g_lbs_semp);
}

/**
 * @brief LBS demonstration task main processing function
 *
 * This function serves as the main loop for the LBS demonstration task, executing the complete LBS positioning process:
 * - Wait for network registration to complete
 * - Establish data connection
 * - Get current cell information
 * - Configure LBS positioning parameters
 * - Execute LBS positioning request
 * - Loop through 100 positioning tests
 *
 * @param[in] arg
 *          - Task parameter
 */
static void unir_lbs_task_handler(void *arg)
{
    int                     ret = 0;
    int                     i = 0, run_num = 1;
    int                     profile_idx = 1;
    qosa_pdp_context_t      pdp_ctx = {0};
    qosa_datacall_conn_t    conn;
    qosa_bool_t             datacall_status = QOSA_FALSE;
    qosa_datacall_ip_info_t info = {0};
    char                    ip4addr_buf[16] = {0};
    qosa_uint8_t            simid = 0;
    qosa_nw_scell_info_t    scell_info = {0};  // Serving cell information
    qosa_uint8_t            reg_status = {0};
    const char             *apn_str = "test";

    // Wait for system startup to complete
    qosa_task_sleep_sec(10);

    QLOGV("==========lbs demo start ==========");
    QLOGV("wait for network register done");

    // Create semaphore for synchronizing location request results
    qosa_sem_create(&g_lbs_semp, 0);

    // Check network registration status, retry up to 10 times
    for (i = 0; i < 10; i++)
    {
        ret = qosa_nw_get_reg_status(simid, QOSA_NULL, &reg_status);
        if ((ret != QOSA_NW_ERR_OK) || (QOSA_FALSE == QOSA_NW_ATTACHED(reg_status)))
        {
            QLOGE("ret: 0x%x, reg_status:%d", ret, reg_status);
            if (i >= 9)
            {
                QLOGE("====network registered failure!!!!====");
                goto exit;
            }
            qosa_task_sleep_sec(1);
        }
        else
        {
            i = 0;
            QLOGV("====network registered!!!!====");
            break;
        }
    }

    // Configure PDP context parameters
    pdp_ctx.apn_valid = QOSA_TRUE;
    pdp_ctx.pdp_type = QOSA_PDP_TYPE_IP;  // ipv4
    if (pdp_ctx.apn_valid)
    {
        qosa_memcpy(pdp_ctx.apn, apn_str, qosa_strlen(apn_str));
    }

    // Set PDP context
    ret = qosa_datacall_set_pdp_context(simid, profile_idx, &pdp_ctx);
    QLOGV("set pdp context, ret=%d", ret);

    // Create data connection object
    conn = qosa_datacall_conn_new(simid, profile_idx, QOSA_DATACALL_CONN_TCPIP);

    // Start data connection (synchronous mode)
    ret = qosa_datacall_start(conn, 120);
    if (ret != QOSA_DATACALL_OK)
    {
        QLOGV("datacall fail ,ret=%d", ret);
        goto exit;
    }

    // Get data connection status
    datacall_status = qosa_datacall_get_status(conn);
    QLOGV("datacall status=%d", datacall_status);

    // Get IP information from data connection
    ret = qosa_datacall_get_ip_info(conn, &info);
    QLOGV("pdpid=%d,simid=%d", info.simcid.pdpid, info.simcid.simid);
    QLOGV("ip_type=%d", info.ip_type);

    // If it is IPv4 type, output IP address
    if (info.ip_type == QOSA_PDP_IPV4)
    {
        // IPv4 information processing
        qosa_memset(ip4addr_buf, 0, sizeof(ip4addr_buf));
        qosa_ip_addr_inet_ntop(QOSA_IP_ADDR_AF_INET, &info.ipv4_ip.addr.ipv4_addr, ip4addr_buf, sizeof(ip4addr_buf));
        QLOGV("ipv4 addr:%s", ip4addr_buf);
    }

    // Loop through 100 LBS positioning tests
    while (run_num <= 100)
    {
        qcm_lbs_option_t user_option;
        QLOGV("==============lbs_test[%d]================", run_num);

        // Get current radio access technology type
        qosa_nw_rat_e cur_rat = qosa_nw_get_current_rat(simid);
        // Get serving cell information
        ret = qosa_nw_get_scell_info(simid, &scell_info);
        if (ret != QOSA_NW_ERR_OK)
        {
            QLOGV("===============lbs get cell info fail===============");
            continue;
        }
        QLOGV("cur_rat:%d", cur_rat);

        // Process cell information according to radio access technology type
        if (cur_rat == QOSA_NW_RAT_2G)
        {
            // 2G network cell information configuration
            g_lbs_cell_info[0].radio = 1;
            g_lbs_cell_info[0].mcc = scell_info.gsm.plmn.mcc;
            g_lbs_cell_info[0].mnc = scell_info.gsm.plmn.mnc;
            g_lbs_cell_info[0].cell_id = scell_info.gsm.cellid;
            g_lbs_cell_info[0].lac_id = scell_info.gsm.lac;
            g_lbs_cell_info[0].bsic = scell_info.gsm.bsic;
            g_lbs_cell_info[0].uarfcndl = scell_info.gsm.arfcn;
            g_lbs_cell_info[0].signal = scell_info.gsm.signal_level;
            QLOGV("cell_id:%x, lac_id:%x, rssi:%d", g_lbs_cell_info[0].cell_id, g_lbs_cell_info[0].lac_id, g_lbs_cell_info[0].signal);
        }
        else if (cur_rat == QOSA_NW_RAT_4G)
        {
            // 4G network cell information configuration
            g_lbs_cell_info[0].radio = 3;
            g_lbs_cell_info[0].mcc = scell_info.lte.plmn.mcc;
            g_lbs_cell_info[0].mnc = scell_info.lte.plmn.mnc;
            g_lbs_cell_info[0].cell_id = scell_info.lte.cellid;
            g_lbs_cell_info[0].lac_id = scell_info.lte.tac;
            g_lbs_cell_info[0].pci = scell_info.lte.pcid;
            g_lbs_cell_info[0].earfcn = scell_info.lte.earfcn;
            g_lbs_cell_info[0].signal = scell_info.lte.rsrp;
            g_lbs_cell_info[0].rsrq = scell_info.lte.rsrq;
            QLOGV(
                "cell_id:%x, lac_id:%x, signal:%d, rsrq:%d",
                g_lbs_cell_info[0].cell_id,
                g_lbs_cell_info[0].lac_id,
                g_lbs_cell_info[0].signal,
                g_lbs_cell_info[0].rsrq
            );
        }
        else
        {
            QLOGV("===============lbs nw get current rat fail===============");
            goto exit;
        }

        // Configure LBS user option parameters
        qosa_memset(&user_option, 0x00, sizeof(qcm_lbs_option_t));
        user_option.pdp_cid = profile_idx;
        user_option.sim_id = 0;
        user_option.req_timeout = 60;
        user_option.basic_info = &g_basic_info;
        user_option.auth_info = &g_auth_info;
        user_option.cell_num = 1;
        user_option.cell_info = &g_lbs_cell_info[0];

        // Apply for LBS client ID
        g_lbs_cli = qcm_lbs_client_new();
        if (g_lbs_cli <= 0)
        {
            QLOGV("lbs client create failed");
            goto exit;
        }

        // Execute LBS positioning request
        if (QCM_LBS_SUCCESS == qcm_lbs_get_position(g_lbs_cli, "www.queclocator.com", &user_option, unir_lbs_result_cb, QOSA_NULL))
        {
            // Wait for positioning result callback to complete
            qosa_sem_wait(g_lbs_semp, QOSA_WAIT_FOREVER);
        }
        else
        {
            QLOGV("lbs failed");
        }
        QLOGV("==============lbs_test_end[%d]================", run_num);
        run_num++;
        // Wait for 1 second before continuing to the next positioning
        qosa_task_sleep_sec(1);
    }

exit:
    qosa_sem_delete(g_lbs_semp);
}

/**
 * @brief Initialize LBS example demonstration, create demonstration task
 *
 * This function is responsible for creating the LBS demonstration task. After the task is created, it will run automatically,
 * executing the complete LBS positioning process to demonstrate LBS functionality.
 *
 * @note Task stack size is 4KB, priority is normal priority
 */

void unir_lbs_demo_init(void)
{
    int err = 0;

    err = qosa_task_create(&g_lbs_task, CONFIG_UNIRTOS_LBS_DEMO_TASK_STACK_SIZE, UNIR_LBS_DEMO_TASK_PRIO, "lbs_demo", unir_lbs_task_handler, QOSA_NULL);
    if (err != QOSA_OK)
    {
        QLOGE("task create error");
        return;
    }
}

UNIRTOS_APP_EXPORT(330, "lbs_demo", unir_lbs_demo_init);