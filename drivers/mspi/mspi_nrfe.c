/*
 * Copyright (c) 2024, Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#define DT_DRV_COMPAT nordic_nrfe_mspi_controller

#include <zephyr/kernel.h>
#include <zephyr/drivers/mspi.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/pm/device.h>
#if !defined(CONFIG_MULTITHREADING)
#include <zephyr/sys/atomic.h>
#endif
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mspi_nrfe, CONFIG_MSPI_LOG_LEVEL);

#include <hal/nrf_gpio.h>
#include <drivers/mspi/nrfe_mspi.h>

#define MSPI_NRFE_NODE	   DT_DRV_INST(0)
#define MAX_TX_MSG_SIZE	   (DT_REG_SIZE(DT_NODELABEL(sram_tx)))
#define MAX_RX_MSG_SIZE	   (DT_REG_SIZE(DT_NODELABEL(sram_rx)))
#define IPC_TIMEOUT_MS	   100
#define EP_SEND_TIMEOUT_MS 10

#define SDP_MPSI_PINCTRL_DEV_CONFIG_INIT(node_id)                                                  \
	{                                                                                          \
		.reg = PINCTRL_REG_NONE,                                                           \
		.states = Z_PINCTRL_STATES_NAME(node_id),                                          \
		.state_cnt = ARRAY_SIZE(Z_PINCTRL_STATES_NAME(node_id)),                           \
	}

#define SDP_MSPI_PINCTRL_DT_DEFINE(node_id)                                                        \
	LISTIFY(DT_NUM_PINCTRL_STATES(node_id), Z_PINCTRL_STATE_PINS_DEFINE, (;), node_id);        \
	Z_PINCTRL_STATES_DEFINE(node_id)                                                           \
	Z_PINCTRL_DEV_CONFIG_STATIC Z_PINCTRL_DEV_CONFIG_CONST struct pinctrl_dev_config           \
	Z_PINCTRL_DEV_CONFIG_NAME(node_id) = SDP_MPSI_PINCTRL_DEV_CONFIG_INIT(node_id)

SDP_MSPI_PINCTRL_DT_DEFINE(MSPI_NRFE_NODE);

static struct ipc_ept ep;
static size_t ipc_received;
static uint8_t *ipc_receive_buffer;
static volatile uint32_t *cpuflpr_error_ctx_ptr =
	(uint32_t *)DT_REG_ADDR(DT_NODELABEL(cpuflpr_error_code));

#if defined(CONFIG_MULTITHREADING)
static K_SEM_DEFINE(ipc_sem, 0, 1);
static K_SEM_DEFINE(ipc_sem_cfg, 0, 1);
static K_SEM_DEFINE(ipc_sem_xfer, 0, 1);
#else
static atomic_t ipc_atomic_sem = ATOMIC_INIT(0);
#endif

#define MSPI_CONFIG                                                                                \
	{                                                                                          \
		.channel_num = 0,                                                                  \
		.op_mode = DT_PROP_OR(MSPI_NRFE_NODE, op_mode, MSPI_OP_MODE_CONTROLLER),           \
		.duplex = DT_PROP_OR(MSPI_NRFE_NODE, duplex, MSPI_FULL_DUPLEX),                    \
		.dqs_support = DT_PROP_OR(MSPI_NRFE_NODE, dqs_support, false),                     \
		.num_periph = DT_CHILD_NUM(MSPI_NRFE_NODE),                                        \
		.max_freq = DT_PROP(MSPI_NRFE_NODE, clock_frequency),                              \
		.re_init = true,                                                                   \
		.sw_multi_periph = false,                                                          \
	}

struct mspi_nrfe_data {
	nrfe_mspi_xfer_config_msg_t xfer_config_msg;
};

struct mspi_nrfe_config {
	struct mspi_cfg mspicfg;
	const struct pinctrl_dev_config *pcfg;
};

static const struct mspi_nrfe_config dev_config = {
	.mspicfg = MSPI_CONFIG,
	.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(0),
};

static struct mspi_nrfe_data dev_data;

static void ep_recv(const void *data, size_t len, void *priv);

static void ep_bound(void *priv)
{
	ipc_received = 0;
#if defined(CONFIG_MULTITHREADING)
	k_sem_give(&ipc_sem);
#else
	atomic_set_bit(&ipc_atomic_sem, NRFE_MSPI_EP_BOUNDED);
#endif
	LOG_DBG("Ep bounded");
}

static struct ipc_ept_cfg ep_cfg = {
	.cb = {.bound = ep_bound, .received = ep_recv},
};

/**
 * @brief IPC receive callback function.
 *
 * This function is called by the IPC stack when a message is received from the
 * other core. The function checks the opcode of the received message and takes
 * appropriate action.
 *
 * @param data Pointer to the received message.
 * @param len Length of the received message.
 */
static void ep_recv(const void *data, size_t len, void *priv)
{
	nrfe_mspi_flpr_response_msg_t *response = (nrfe_mspi_flpr_response_msg_t *)data;

	switch (response->opcode) {
#if defined(CONFIG_MSPI_NRFE_FAULT_TIMER)
	case NRFE_MSPI_CONFIG_TIMER_PTR: {
#if defined(CONFIG_MULTITHREADING)
		k_sem_give(&ipc_sem);
#else
		atomic_set_bit(&ipc_atomic_sem, NRFE_MSPI_CONFIG_TIMER_PTR);
#endif
		break;
	}
#endif
	case NRFE_MSPI_CONFIG_PINS: {
#if defined(CONFIG_MULTITHREADING)
		k_sem_give(&ipc_sem_cfg);
#else
		atomic_set_bit(&ipc_atomic_sem, NRFE_MSPI_CONFIG_PINS);
#endif
		break;
	}
	case NRFE_MSPI_CONFIG_DEV: {
#if defined(CONFIG_MULTITHREADING)
		k_sem_give(&ipc_sem_cfg);
#else
		atomic_set_bit(&ipc_atomic_sem, NRFE_MSPI_CONFIG_DEV);
#endif
		break;
	}
	case NRFE_MSPI_CONFIG_XFER: {
#if defined(CONFIG_MULTITHREADING)
		k_sem_give(&ipc_sem_cfg);
#else
		atomic_set_bit(&ipc_atomic_sem, NRFE_MSPI_CONFIG_XFER);
#endif
		break;
	}
	case NRFE_MSPI_TX: {
#if defined(CONFIG_MULTITHREADING)
		k_sem_give(&ipc_sem_xfer);
#else
		atomic_set_bit(&ipc_atomic_sem, NRFE_MSPI_TX);
#endif
		break;
	}
	case NRFE_MSPI_TXRX: {
		if (len > 0) {
			ipc_received = len - sizeof(nrfe_mspi_opcode_t);
			ipc_receive_buffer = (uint8_t *)&response->data;
		}
#if defined(CONFIG_MULTITHREADING)
		k_sem_give(&ipc_sem_xfer);
#else
		atomic_set_bit(&ipc_atomic_sem, NRFE_MSPI_TXRX);
#endif
		break;
	}
	case NRFE_MSPI_SDP_APP_HARD_FAULT: {

		volatile uint32_t cause = cpuflpr_error_ctx_ptr[0];
		volatile uint32_t pc = cpuflpr_error_ctx_ptr[1];
		volatile uint32_t bad_addr = cpuflpr_error_ctx_ptr[2];
		volatile uint32_t *ctx = (volatile uint32_t *)cpuflpr_error_ctx_ptr[3];

		LOG_ERR(">>> SDP APP FATAL ERROR");
		LOG_ERR("Faulting instruction address (mepc): 0x%08x", pc);
		LOG_ERR("mcause: 0x%08x, mtval: 0x%08x, ra: 0x%08x", cause, bad_addr, ctx[0]);
		LOG_ERR("    t0: 0x%08x,    t1: 0x%08x, t2: 0x%08x", ctx[1], ctx[2], ctx[3]);

		LOG_ERR("SDP application halted...");
		break;
	}
	default: {
		LOG_ERR("Invalid response opcode: %d", response->opcode);
		break;
	}
	}

	LOG_HEXDUMP_DBG((uint8_t *)data, len, "Received msg:");
}

/**
 * @brief Send data to the flpr with the given opcode.
 *
 * @param opcode The opcode of the message to send.
 * @param data The data to send.
 * @param len The length of the data to send.
 *
 * @return 0 on success, -ENOMEM if there is no space in the buffer,
 *         -ETIMEDOUT if the transfer timed out.
 */
static int mspi_ipc_data_send(nrfe_mspi_opcode_t opcode, const void *data, size_t len)
{
	int rc;

	LOG_DBG("Sending msg with opcode: %d", (uint8_t)opcode);
#if defined(CONFIG_SYS_CLOCK_EXISTS)
	uint32_t start = k_uptime_get_32();
#else
	uint32_t repeat = EP_SEND_TIMEOUT_MS;
#endif
#if !defined(CONFIG_MULTITHREADING)
	atomic_clear_bit(&ipc_atomic_sem, opcode);
#endif

	do {
		rc = ipc_service_send(&ep, data, len);
#if defined(CONFIG_SYS_CLOCK_EXISTS)
		if ((k_uptime_get_32() - start) > EP_SEND_TIMEOUT_MS) {
#else
		repeat--;
		if ((rc < 0) && (repeat == 0)) {
#endif
			break;
		};
	} while (rc == -ENOMEM); /* No space in the buffer. Retry. */

	return rc;
}

/**
 * @brief Waits for a response from the peer with the given opcode.
 *
 * @param opcode The opcode of the response to wait for.
 * @param timeout The timeout in milliseconds.
 *
 * @return 0 on success, -ETIMEDOUT if the operation timed out.
 */
static int nrfe_mspi_wait_for_response(nrfe_mspi_opcode_t opcode, uint32_t timeout)
{
#if defined(CONFIG_MULTITHREADING)
	int ret = 0;

	switch (opcode) {
	case NRFE_MSPI_CONFIG_TIMER_PTR:
		ret = k_sem_take(&ipc_sem, K_MSEC(timeout));
		break;
	case NRFE_MSPI_CONFIG_PINS:
	case NRFE_MSPI_CONFIG_DEV:
	case NRFE_MSPI_CONFIG_XFER: {
		ret = k_sem_take(&ipc_sem_cfg, K_MSEC(timeout));
		break;
	}
	case NRFE_MSPI_TX:
	case NRFE_MSPI_TXRX:
		ret = k_sem_take(&ipc_sem_xfer, K_MSEC(timeout));
		break;
	default:
		break;
	}

	if (ret < 0) {
		return -ETIMEDOUT;
	}
#else
#if defined(CONFIG_SYS_CLOCK_EXISTS)
	uint32_t start = k_uptime_get_32();
#else
	uint32_t repeat = timeout * 1000; /* Convert ms to us */
#endif
	while (!atomic_test_and_clear_bit(&ipc_atomic_sem, opcode)) {
#if defined(CONFIG_SYS_CLOCK_EXISTS)
		if ((k_uptime_get_32() - start) > timeout) {
			return -ETIMEDOUT;
		};
#else
		repeat--;
		if (!repeat) {
			return -ETIMEDOUT;
		};
#endif
		k_sleep(K_USEC(1));
	}
#endif /* CONFIG_MULTITHREADING */
	return 0;
}

/**
 * @brief Send data to the FLPR core using the IPC service, and wait for FLPR response.
 *
 * @param opcode The configuration packet opcode to send.
 * @param data The data to send.
 * @param len The length of the data to send.
 *
 * @return 0 on success, negative errno code on failure.
 */
static int send_data(nrfe_mspi_opcode_t opcode, const void *data, size_t len)
{
	int rc;

#ifdef CONFIG_MSPI_NRFE_IPC_NO_COPY
	(void)len;
	void *data_ptr = (void *)data;

	rc = mspi_ipc_data_send(opcode, &data_ptr, sizeof(void *));
#else
	rc = mspi_ipc_data_send(opcode, data, len);
#endif

	if (rc < 0) {
		LOG_ERR("Data transfer failed: %d", rc);
		return rc;
	}

	rc = nrfe_mspi_wait_for_response(opcode, IPC_TIMEOUT_MS);
	if (rc < 0) {
		LOG_ERR("Data transfer: %d response timeout: %d!", opcode, rc);
	}

	return rc;
}

/**
 * @brief Configures the MSPI controller based on the provided spec.
 *
 * This function configures the MSPI controller according to the provided
 * spec. It checks if the spec is valid and sends the configuration to
 * the FLPR.
 *
 * @param spec The MSPI spec to use for configuration.
 *
 * @return 0 on success, negative errno code on failure.
 */
static int api_config(const struct mspi_dt_spec *spec)
{
	const struct mspi_cfg *config = &spec->config;
	const struct mspi_nrfe_config *drv_cfg = spec->bus->config;
	nrfe_mspi_pinctrl_soc_pin_msg_t mspi_pin_config;

	if (config->op_mode != MSPI_OP_MODE_CONTROLLER) {
		LOG_ERR("Only MSPI controller mode is supported.");
		return -ENOTSUP;
	}

	if (config->dqs_support) {
		LOG_ERR("DQS mode is not supported.");
		return -ENOTSUP;
	}

	if (config->max_freq > drv_cfg->mspicfg.max_freq) {
		LOG_ERR("max_freq is too large.");
		return -ENOTSUP;
	}

	/* Create pinout configuration */
	uint8_t state_id;

	for (state_id = 0; state_id < drv_cfg->pcfg->state_cnt; state_id++) {
		if (drv_cfg->pcfg->states[state_id].id == PINCTRL_STATE_DEFAULT) {
			break;
		}
	}

	if (drv_cfg->pcfg->states[state_id].pin_cnt > NRFE_MSPI_PINS_MAX) {
		LOG_ERR("Too many pins defined. Max: %d", NRFE_MSPI_PINS_MAX);
		return -ENOTSUP;
	}

	if (drv_cfg->pcfg->states[state_id].id != PINCTRL_STATE_DEFAULT) {
		LOG_ERR("Pins default state not found.");
		return -ENOTSUP;
	}

	for (uint8_t i = 0; i < drv_cfg->pcfg->states[state_id].pin_cnt; i++) {
		mspi_pin_config.pin[i] = drv_cfg->pcfg->states[state_id].pins[i];
	}
	mspi_pin_config.opcode = NRFE_MSPI_CONFIG_PINS;

	/* Send pinout configuration to FLPR */
	return send_data(NRFE_MSPI_CONFIG_PINS, (const void *)&mspi_pin_config,
			 sizeof(nrfe_mspi_pinctrl_soc_pin_msg_t));
}

static int check_io_mode(enum mspi_io_mode io_mode)
{
	switch (io_mode) {
	case MSPI_IO_MODE_SINGLE:
	case MSPI_IO_MODE_QUAD:
	case MSPI_IO_MODE_QUAD_1_1_4:
	case MSPI_IO_MODE_QUAD_1_4_4:
		break;
	default:
		LOG_ERR("IO mode %d not supported", io_mode);
		return -ENOTSUP;
	}

	return 0;
}

/**
 * @brief Configure a device on the MSPI bus.
 *
 * @param dev MSPI controller device.
 * @param dev_id Device ID to configure.
 * @param param_mask Bitmask of parameters to configure.
 * @param cfg Device configuration.
 *
 * @return 0 on success, negative errno code on failure.
 */
static int api_dev_config(const struct device *dev, const struct mspi_dev_id *dev_id,
			  const enum mspi_dev_cfg_mask param_mask, const struct mspi_dev_cfg *cfg)
{
	const struct mspi_nrfe_config *drv_cfg = dev->config;
	int rc;
	nrfe_mspi_dev_config_msg_t mspi_dev_config_msg;

	if (param_mask & MSPI_DEVICE_CONFIG_MEM_BOUND) {
		if (cfg->mem_boundary) {
			LOG_ERR("Memory boundary is not supported.");
			return -ENOTSUP;
		}
	}

	if (param_mask & MSPI_DEVICE_CONFIG_BREAK_TIME) {
		if (cfg->time_to_break) {
			LOG_ERR("Transfer break is not supported.");
			return -ENOTSUP;
		}
	}

	if (param_mask & MSPI_DEVICE_CONFIG_FREQUENCY) {
		if (cfg->freq > drv_cfg->mspicfg.max_freq) {
			LOG_ERR("Invalid frequency: %u, MAX: %u", cfg->freq,
				drv_cfg->mspicfg.max_freq);
			return -EINVAL;
		}
	}

	if (param_mask & MSPI_DEVICE_CONFIG_IO_MODE) {
		rc = check_io_mode(cfg->io_mode);
		if (rc < 0) {
			return rc;
		}
	}

	if (param_mask & MSPI_DEVICE_CONFIG_DATA_RATE) {
		if (cfg->data_rate != MSPI_DATA_RATE_SINGLE) {
			LOG_ERR("Only single data rate is supported.");
			return -ENOTSUP;
		}
	}

	if (param_mask & MSPI_DEVICE_CONFIG_DQS) {
		if (cfg->dqs_enable) {
			LOG_ERR("DQS signal is not supported.");
			return -ENOTSUP;
		}
	}

	mspi_dev_config_msg.opcode = NRFE_MSPI_CONFIG_DEV;
	mspi_dev_config_msg.device_index = dev_id->dev_idx;
	mspi_dev_config_msg.dev_config.io_mode = cfg->io_mode;
	mspi_dev_config_msg.dev_config.cpp = cfg->cpp;
	mspi_dev_config_msg.dev_config.ce_polarity = cfg->ce_polarity;
	mspi_dev_config_msg.dev_config.freq = cfg->freq;
	mspi_dev_config_msg.dev_config.ce_index = cfg->ce_num;

	return send_data(NRFE_MSPI_CONFIG_DEV, (void *)&mspi_dev_config_msg,
			 sizeof(nrfe_mspi_dev_config_msg_t));
}

static int api_get_channel_status(const struct device *dev, uint8_t ch)
{
	return 0;
}

/**
 * @brief Send a transfer packet to the eMSPI controller.
 *
 * @param dev eMSPI controller device
 * @param packet Transfer packet containing the data to be transferred
 * @param timeout Timeout in milliseconds
 *
 * @retval 0 on success
 * @retval -ENOTSUP if the packet is not supported
 * @retval -ENOMEM if there is no space in the buffer
 * @retval -ETIMEDOUT if the transfer timed out
 */
static int xfer_packet(struct mspi_xfer_packet *packet, uint32_t timeout)
{
	int rc;
	nrfe_mspi_opcode_t opcode = (packet->dir == MSPI_RX) ? NRFE_MSPI_TXRX : NRFE_MSPI_TX;

#ifdef CONFIG_MSPI_NRFE_IPC_NO_COPY
	/* Check for alignment problems. */
	uint32_t len = ((uint32_t)packet->data_buf) % sizeof(uint32_t) != 0
			       ? sizeof(nrfe_mspi_xfer_packet_msg_t) + packet->num_bytes
			       : sizeof(nrfe_mspi_xfer_packet_msg_t);
#else
	uint32_t len = sizeof(nrfe_mspi_xfer_packet_msg_t) + packet->num_bytes;
#endif
	uint8_t buffer[len];
	nrfe_mspi_xfer_packet_msg_t *xfer_packet = (nrfe_mspi_xfer_packet_msg_t *)buffer;

	xfer_packet->opcode = opcode;
	xfer_packet->command = packet->cmd;
	xfer_packet->address = packet->address;
	xfer_packet->num_bytes = packet->num_bytes;

#ifdef CONFIG_MSPI_NRFE_IPC_NO_COPY
	/* Check for alignlemt problems. */
	if (((uint32_t)packet->data_buf) % sizeof(uint32_t) != 0) {
		memcpy((void *)(buffer + sizeof(nrfe_mspi_xfer_packet_msg_t)),
		       (void *)packet->data_buf, packet->num_bytes);
		xfer_packet->data = buffer + sizeof(nrfe_mspi_xfer_packet_msg_t);
	} else {
		xfer_packet->data = packet->data_buf;
	}
#else
	memcpy((void *)xfer_packet->data, (void *)packet->data_buf, packet->num_bytes);
#endif

	rc = send_data(xfer_packet->opcode, xfer_packet, len);

	/* Wait for the transfer to complete and receive data. */
	if ((packet->dir == MSPI_RX) && (ipc_receive_buffer != NULL) && (ipc_received > 0)) {
		/*
		 * It is not possible to check whether received data is valid, so packet->num_bytes
		 * should always be equal to ipc_received. If it is not, then something went wrong.
		 */
		if (packet->num_bytes != ipc_received) {
			rc = -EIO;
		} else {
			memcpy((void *)packet->data_buf, (void *)ipc_receive_buffer, ipc_received);
		}

		/* Clear the receive buffer pointer and size */
		ipc_receive_buffer = NULL;
		ipc_received = 0;
	}

	return rc;
}

/**
 * @brief Initiates the transfer of the next packet in an MSPI transaction.
 *
 * This function prepares and starts the transmission of the next packet
 * specified in the MSPI transfer configuration. It checks if the packet
 * size is within the allowable limits before initiating the transfer.
 *
 * @param xfer Pointer to the mspi_xfer structure.
 * @param packets_done Number of packets that have already been processed.
 *
 * @retval 0 If the packet transfer is successfully started.
 * @retval -EINVAL If the packet size exceeds the maximum transmission size.
 */
static int start_next_packet(struct mspi_xfer *xfer, uint32_t packets_done)
{
	struct mspi_xfer_packet *packet = (struct mspi_xfer_packet *)&xfer->packets[packets_done];

	if (packet->num_bytes >= MAX_TX_MSG_SIZE) {
		LOG_ERR("Packet size to large: %u. Increase SRAM data region.", packet->num_bytes);
		return -EINVAL;
	}

	return xfer_packet(packet, xfer->timeout);
}

/**
 * @brief Send a multi-packet transfer request to the host.
 *
 * This function sends a multi-packet transfer request to the host and waits
 * for the host to complete the transfer. This function does not support
 * asynchronous transfers.
 *
 * @param dev Pointer to the device structure.
 * @param dev_id Pointer to the device identification structure.
 * @param req Pointer to the xfer structure.
 *
 * @retval 0 If successful.
 * @retval -ENOTSUP If asynchronous transfers are requested.
 * @retval -EIO If an I/O error occurs.
 */
static int api_transceive(const struct device *dev, const struct mspi_dev_id *dev_id,
			  const struct mspi_xfer *req)
{
	struct mspi_nrfe_data *drv_data = dev->data;
	uint32_t packets_done = 0;
	int rc;

	/* TODO: add support for asynchronous transfers */
	if (req->async) {
		return -ENOTSUP;
	}

	if (req->num_packet == 0 || !req->packets ||
	    req->timeout > CONFIG_MSPI_COMPLETION_TIMEOUT_TOLERANCE) {
		return -EFAULT;
	}

	drv_data->xfer_config_msg.opcode = NRFE_MSPI_CONFIG_XFER;
	drv_data->xfer_config_msg.xfer_config.device_index = dev_id->dev_idx;
	drv_data->xfer_config_msg.xfer_config.command_length = req->cmd_length;
	drv_data->xfer_config_msg.xfer_config.address_length = req->addr_length;
	drv_data->xfer_config_msg.xfer_config.hold_ce = req->hold_ce;
	drv_data->xfer_config_msg.xfer_config.tx_dummy = req->tx_dummy;
	drv_data->xfer_config_msg.xfer_config.rx_dummy = req->rx_dummy;

	rc = send_data(NRFE_MSPI_CONFIG_XFER, (void *)&drv_data->xfer_config_msg,
		       sizeof(nrfe_mspi_xfer_config_msg_t));

	if (rc < 0) {
		LOG_ERR("Send xfer config error: %d", rc);
		return rc;
	}

	while (packets_done < req->num_packet) {
		rc = start_next_packet((struct mspi_xfer *)req, packets_done);
		if (rc < 0) {
			LOG_ERR("Start next packet error: %d", rc);
			return rc;
		}
		++packets_done;
	}

	return 0;
}

#if CONFIG_PM_DEVICE
/**
 * @brief Callback function to handle power management actions.
 *
 * This function is responsible for handling power management actions
 * such as suspend and resume for the given device. It performs the
 * necessary operations when the device is requested to transition
 * between different power states.
 *
 * @param dev Pointer to the device structure.
 * @param action The power management action to be performed.
 *
 * @retval 0 If successful.
 * @retval -ENOTSUP If the action is not supported.
 */
static int dev_pm_action_cb(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		/* TODO: Handle PM suspend state */
		break;
	case PM_DEVICE_ACTION_RESUME:
		/* TODO: Handle PM resume state */
		break;
	default:
		return -ENOTSUP;
	}

	return 0;
}
#endif

#if defined(CONFIG_MSPI_NRFE_FAULT_TIMER)
static void flpr_fault_handler(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	LOG_ERR("SDP fault detected.");
}
#endif

/**
 * @brief Initialize the MSPI NRFE driver.
 *
 * This function initializes the MSPI NRFE driver. It is responsible for
 * setting up the hardware and registering the IPC endpoint for the
 * driver.
 *
 * @param dev Pointer to the device structure for the MSPI NRFE driver.
 *
 * @retval 0 If successful.
 * @retval -errno If an error occurs.
 */
static int nrfe_mspi_init(const struct device *dev)
{
	int ret;
	const struct device *ipc_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));
	const struct mspi_nrfe_config *drv_cfg = dev->config;
	const struct mspi_dt_spec spec = {
		.bus = dev,
		.config = drv_cfg->mspicfg,
	};

#if defined(CONFIG_MSPI_NRFE_FAULT_TIMER)
	const struct device *const flpr_fault_timer = DEVICE_DT_GET(DT_NODELABEL(fault_timer));
	const struct counter_top_cfg top_cfg = {
		.callback = flpr_fault_handler,
		.user_data = NULL,
		.flags = 0,
		.ticks = counter_us_to_ticks(flpr_fault_timer, CONFIG_MSPI_NRFE_FAULT_TIMEOUT)
	};
#endif

	ret = pinctrl_apply_state(drv_cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret) {
		return ret;
	}

	ret = ipc_service_open_instance(ipc_instance);
	if ((ret < 0) && (ret != -EALREADY)) {
		LOG_ERR("ipc_service_open_instance() failure");
		return ret;
	}

	ret = ipc_service_register_endpoint(ipc_instance, &ep, &ep_cfg);
	if (ret < 0) {
		LOG_ERR("ipc_service_register_endpoint() failure");
		return ret;
	}

	/* Wait for ep to be bounded */
#if defined(CONFIG_MULTITHREADING)
	k_sem_take(&ipc_sem, K_FOREVER);
#else
	while (!atomic_test_and_clear_bit(&ipc_atomic_sem, NRFE_MSPI_EP_BOUNDED)) {
	}
#endif

	ret = api_config(&spec);
	if (ret < 0) {
		return ret;
	}

#if CONFIG_PM_DEVICE
	ret = pm_device_driver_init(dev, dev_pm_action_cb);
	if (ret < 0) {
		return ret;
	}
#endif

#if defined(CONFIG_MSPI_NRFE_FAULT_TIMER)
	/* Configure timer as SDP `watchdog` */
	if (!device_is_ready(flpr_fault_timer)) {
		LOG_ERR("FLPR timer not ready");
		return -1;
	}

	ret = counter_set_top_value(flpr_fault_timer, &top_cfg);
	if (ret < 0) {
		LOG_ERR("counter_set_top_value() failure");
		return ret;
	}

	/* Send timer address to FLPR */
	nrfe_mspi_flpr_timer_msg_t timer_data = {
		.opcode = NRFE_MSPI_CONFIG_TIMER_PTR,
		.timer_ptr = (NRF_TIMER_Type *)DT_REG_ADDR(DT_NODELABEL(fault_timer)),
	};

	ret = send_data(NRFE_MSPI_CONFIG_TIMER_PTR, (const void *)&timer_data.opcode,
			sizeof(nrfe_mspi_flpr_timer_msg_t));
	if (ret < 0) {
		LOG_ERR("Send timer configuration failure");
		return ret;
	}

	ret = counter_start(flpr_fault_timer);
	if (ret < 0) {
		LOG_ERR("counter_start() failure");
		return ret;
	}
#endif

	return ret;
}

static const struct mspi_driver_api drv_api = {
	.config = api_config,
	.dev_config = api_dev_config,
	.get_channel_status = api_get_channel_status,
	.transceive = api_transceive,
};

PM_DEVICE_DT_INST_DEFINE(0, dev_pm_action_cb);

DEVICE_DT_INST_DEFINE(0, nrfe_mspi_init, PM_DEVICE_DT_INST_GET(0), &dev_data, &dev_config,
		      POST_KERNEL, CONFIG_MSPI_NRFE_INIT_PRIORITY, &drv_api);
