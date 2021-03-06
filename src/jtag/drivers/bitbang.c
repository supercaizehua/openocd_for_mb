/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007,2008 Øyvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

/* 2014-12: Addition of the SWD protocol support is based on the initial work
 * by Paul Fertser and modifications by Jean-Christian de Rivaz. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h> 
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>

#include "bitbang.h"
#include <jtag/interface.h>
#include <jtag/commands.h>

/* YUK! - but this is currently a global.... */
extern struct jtag_interface *jtag_interface;

/**
 * Function bitbang_stableclocks
 * issues a number of clock cycles while staying in a stable state.
 * Because the TMS value required to stay in the RESET state is a 1, whereas
 * the TMS value required to stay in any of the other stable states is a 0,
 * this function checks the current stable state to decide on the value of TMS
 * to use.
 */
static void bitbang_stableclocks(int num_cycles);

static void bitbang_swd_write_reg(uint8_t cmd, uint32_t value, uint32_t ap_delay_clk);

struct bitbang_interface *bitbang_interface;

/* DANGER!!!! clock absolutely *MUST* be 0 in idle or reset won't work!
 *
 * Set this to 1 and str912 reset halt will fail.
 *
 * If someone can submit a patch with an explanation it will be greatly
 * appreciated, but as far as I can tell (ØH) DCLK is generated upon
 * clk = 0 in TAP_IDLE. Good luck deducing that from the ARM documentation!
 * The ARM documentation uses the term "DCLK is asserted while in the TAP_IDLE
 * state". With hardware there is no such thing as *while* in a state. There
 * are only edges. So clk => 0 is in fact a very subtle state transition that
 * happens *while* in the TAP_IDLE state. "#&¤"#¤&"#&"#&
 *
 * For "reset halt" the last thing that happens before srst is asserted
 * is that the breakpoint is set up. If DCLK is not wiggled one last
 * time before the reset, then the breakpoint is not set up and
 * "reset halt" will fail to halt.
 *
 */
#define CLOCK_IDLE() 0

/* The bitbang driver leaves the TCK 0 when in idle */
static void bitbang_end_state(tap_state_t state)
{
	if (tap_is_state_stable(state))
		tap_set_end_state(state);
	else {
		LOG_ERROR("BUG: %i is not a valid end state", state);
		exit(-1);
	}
}

static void bitbang_state_move(int skip)
{
	int i = 0, tms = 0;
	uint8_t tms_scan = tap_get_tms_path(tap_get_state(), tap_get_end_state());
	int tms_count = tap_get_tms_path_len(tap_get_state(), tap_get_end_state());

	for (i = skip; i < tms_count; i++) {
		tms = (tms_scan >> i) & 1;
		bitbang_interface->write(0, tms, 0);
		bitbang_interface->write(1, tms, 0);
	}
	bitbang_interface->write(CLOCK_IDLE(), tms, 0);

	tap_set_state(tap_get_end_state());
}

/**
 * Clock a bunch of TMS (or SWDIO) transitions, to change the JTAG
 * (or SWD) state machine.
 */
static int bitbang_execute_tms(struct jtag_command *cmd)
{
	unsigned num_bits = cmd->cmd.tms->num_bits;
	const uint8_t *bits = cmd->cmd.tms->bits;

	DEBUG_JTAG_IO("TMS: %d bits", num_bits);

	int tms = 0;
	for (unsigned i = 0; i < num_bits; i++) {
		tms = ((bits[i/8] >> (i % 8)) & 1);
		bitbang_interface->write(0, tms, 0);
		bitbang_interface->write(1, tms, 0);
	}
	bitbang_interface->write(CLOCK_IDLE(), tms, 0);

	return ERROR_OK;
}

static void bitbang_path_move(struct pathmove_command *cmd)
{
	int num_states = cmd->num_states;
	int state_count;
	int tms = 0;

	state_count = 0;
	while (num_states) {
		if (tap_state_transition(tap_get_state(), false) == cmd->path[state_count])
			tms = 0;
		else if (tap_state_transition(tap_get_state(), true) == cmd->path[state_count])
			tms = 1;
		else {
			LOG_ERROR("BUG: %s -> %s isn't a valid TAP transition",
				tap_state_name(tap_get_state()),
				tap_state_name(cmd->path[state_count]));
			exit(-1);
		}

		bitbang_interface->write(0, tms, 0);
		bitbang_interface->write(1, tms, 0);

		tap_set_state(cmd->path[state_count]);
		state_count++;
		num_states--;
	}

	bitbang_interface->write(CLOCK_IDLE(), tms, 0);

	tap_set_end_state(tap_get_state());
}

static void bitbang_runtest(int num_cycles)
{
	int i;

	tap_state_t saved_end_state = tap_get_end_state();

	/* only do a state_move when we're not already in IDLE */
	if (tap_get_state() != TAP_IDLE) {
		bitbang_end_state(TAP_IDLE);
		bitbang_state_move(0);
	}

	/* execute num_cycles */
	for (i = 0; i < num_cycles; i++) {
		bitbang_interface->write(0, 0, 0);
		bitbang_interface->write(1, 0, 0);
	}
	bitbang_interface->write(CLOCK_IDLE(), 0, 0);

	/* finish in end_state */
	bitbang_end_state(saved_end_state);
	if (tap_get_state() != tap_get_end_state())
		bitbang_state_move(0);
}

static void bitbang_stableclocks(int num_cycles)
{
	int tms = (tap_get_state() == TAP_RESET ? 1 : 0);
	int i;

	/* send num_cycles clocks onto the cable */
	for (i = 0; i < num_cycles; i++) {
		bitbang_interface->write(1, tms, 0);
		bitbang_interface->write(0, tms, 0);
	}
}

static void bitbang_scan(bool ir_scan, enum scan_type type, uint8_t *buffer, int scan_size)
{
	tap_state_t saved_end_state = tap_get_end_state();
	int bit_cnt;

	if (!((!ir_scan &&
			(tap_get_state() == TAP_DRSHIFT)) ||
			(ir_scan && (tap_get_state() == TAP_IRSHIFT)))) {
		if (ir_scan)
			bitbang_end_state(TAP_IRSHIFT);
		else
			bitbang_end_state(TAP_DRSHIFT);

		bitbang_state_move(0);
		bitbang_end_state(saved_end_state);
	}

	for (bit_cnt = 0; bit_cnt < scan_size; bit_cnt++) {
		int val = 0;
		int tms = (bit_cnt == scan_size-1) ? 1 : 0;
		int tdi;
		int bytec = bit_cnt/8;
		int bcval = 1 << (bit_cnt % 8);

		/* if we're just reading the scan, but don't care about the output
		 * default to outputting 'low', this also makes valgrind traces more readable,
		 * as it removes the dependency on an uninitialised value
		 */
		tdi = 0;
		if ((type != SCAN_IN) && (buffer[bytec] & bcval))
			tdi = 1;

		bitbang_interface->write(0, tms, tdi);

		if (type != SCAN_OUT)
			val = bitbang_interface->read();

		bitbang_interface->write(1, tms, tdi);

		if (type != SCAN_OUT) {
			if (val)
				buffer[bytec] |= bcval;
			else
				buffer[bytec] &= ~bcval;
		}
	}

	if (tap_get_state() != tap_get_end_state()) {
		/* we *KNOW* the above loop transitioned out of
		 * the shift state, so we skip the first state
		 * and move directly to the end state.
		 */
		bitbang_state_move(1);
	}
}

int bitbang_execute_queue(void)
{
	struct jtag_command *cmd = jtag_command_queue;	/* currently processed command */
	int scan_size;
	enum scan_type type;
	uint8_t *buffer;
	int retval;

	if (!bitbang_interface) {
		LOG_ERROR("BUG: Bitbang interface called, but not yet initialized");
		exit(-1);
	}

	/* return ERROR_OK, unless a jtag_read_buffer returns a failed check
	 * that wasn't handled by a caller-provided error handler
	 */
	retval = ERROR_OK;

	if (bitbang_interface->blink)
		bitbang_interface->blink(1);

	while (cmd) {
		switch (cmd->type) {
			case JTAG_RESET:
#ifdef _DEBUG_JTAG_IO_
				LOG_DEBUG("reset trst: %i srst %i",
				cmd->cmd.reset->trst,
				cmd->cmd.reset->srst);
#endif
				if ((cmd->cmd.reset->trst == 1) ||
						(cmd->cmd.reset->srst && (jtag_get_reset_config() & RESET_SRST_PULLS_TRST)))
					tap_set_state(TAP_RESET);
				bitbang_interface->reset(cmd->cmd.reset->trst, cmd->cmd.reset->srst);
				break;
			case JTAG_RUNTEST:
#ifdef _DEBUG_JTAG_IO_
				LOG_DEBUG("runtest %i cycles, end in %s",
						cmd->cmd.runtest->num_cycles,
						tap_state_name(cmd->cmd.runtest->end_state));
#endif
				bitbang_end_state(cmd->cmd.runtest->end_state);
				bitbang_runtest(cmd->cmd.runtest->num_cycles);
				break;

			case JTAG_STABLECLOCKS:
				/* this is only allowed while in a stable state.  A check for a stable
				 * state was done in jtag_add_clocks()
				 */
				bitbang_stableclocks(cmd->cmd.stableclocks->num_cycles);
				break;

			case JTAG_TLR_RESET:
#ifdef _DEBUG_JTAG_IO_
				LOG_DEBUG("statemove end in %s",
						tap_state_name(cmd->cmd.statemove->end_state));
#endif
				bitbang_end_state(cmd->cmd.statemove->end_state);
				bitbang_state_move(0);
				break;
			case JTAG_PATHMOVE:
#ifdef _DEBUG_JTAG_IO_
				LOG_DEBUG("pathmove: %i states, end in %s",
						cmd->cmd.pathmove->num_states,
						tap_state_name(cmd->cmd.pathmove->path[cmd->cmd.pathmove->num_states - 1]));
#endif
				bitbang_path_move(cmd->cmd.pathmove);
				break;
			case JTAG_SCAN:
#ifdef _DEBUG_JTAG_IO_
				LOG_DEBUG("%s scan end in %s",
						(cmd->cmd.scan->ir_scan) ? "IR" : "DR",
					tap_state_name(cmd->cmd.scan->end_state));
#endif
				bitbang_end_state(cmd->cmd.scan->end_state);
				scan_size = jtag_build_buffer(cmd->cmd.scan, &buffer);
				type = jtag_scan_type(cmd->cmd.scan);
				bitbang_scan(cmd->cmd.scan->ir_scan, type, buffer, scan_size);
				if (jtag_read_buffer(buffer, cmd->cmd.scan) != ERROR_OK)
					retval = ERROR_JTAG_QUEUE_FAILED;
				if (buffer)
					free(buffer);
				break;
			case JTAG_SLEEP:
#ifdef _DEBUG_JTAG_IO_
				LOG_DEBUG("sleep %" PRIi32, cmd->cmd.sleep->us);
#endif
				jtag_sleep(cmd->cmd.sleep->us);
				break;
			case JTAG_TMS:
				retval = bitbang_execute_tms(cmd);
				break;
			default:
				LOG_ERROR("BUG: unknown JTAG command type encountered");
				exit(-1);
		}
		cmd = cmd->next;
	}
	if (bitbang_interface->blink)
		bitbang_interface->blink(0);

	return retval;
}


bool swd_mode;
static int queued_retval;

int set_interface_attribs (int fd, int speed, int parity)
{
    struct termios tty;
    memset (&tty, 0, sizeof tty);
    if (tcgetattr (fd, &tty) != 0)
    {
        printf ("error %d from tcgetattr", errno);
        return -1;
    }

    cfsetospeed (&tty, speed);
    cfsetispeed (&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
    // disable IGNBRK for mismatched speed tests; otherwise receive break
    // as \000 chars
    tty.c_iflag &= ~IGNBRK;         // disable break processing
    tty.c_lflag = 0;                // no signaling chars, no echo,
    // no canonical processing
    tty.c_oflag = 0;                // no remapping, no delays
    tty.c_cc[VMIN]  = 1;            // read doesn't block
    tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

    tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls,
    // enable reading
    tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
    tty.c_cflag |= parity;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr (fd, TCSANOW, &tty) != 0)
    {
        printf ("error %d from tcsetattr", errno);
        return -1;
    }
    return 0;
}

void
set_blocking (int fd, int should_block)
{
    struct termios tty;
    memset (&tty, 0, sizeof tty);
    if (tcgetattr (fd, &tty) != 0)
    {
        printf ("error %d from tggetattr", errno);
        return;
    }

    tty.c_cc[VMIN]  = should_block ? 1 : 0;
    tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

    if (tcsetattr (fd, TCSANOW, &tty) != 0)
        printf ("error %d setting term attributes", errno);
}
int fd;



static int bitbang_swd_init(void)
{
	LOG_DEBUG("bitbang_swd_init");
	swd_mode = true;
    char *portname = "/dev/ttyACM0";
    fd = open (portname, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0)
    {
        printf ("error %d opening %s: %s", errno, portname, strerror (errno));
        return ERROR_FAIL;
    }
    set_interface_attribs (fd, B115200, 0);  // set speed to 115,200 bps, 8n1 (no parity)
    set_blocking (fd, 0);                // set no blocking
    
	return ERROR_OK;
}

#define GET_UPPER_HEX(x) ((((x) >> 4) & 0x0F) > 9 ? ((((x) >> 4) & 0x0F) - 10 + 'A') : ((((x) >> 4) & 0x0F) + '0'))
#define GET_LOWER_HEX(x) (((x) & 0x0F) > 9 ? (((x) & 0x0F) - 10 + 'A') : (((x) & 0x0F) + '0'))
#define HEX_TO_INT(x) ((x) >= 'A' ? ((x) - 'A' + 10) : ((x) - '0'))
uint8_t local_buf[1024];

#define PRINTF(x, ...) do {                     \
    }while(0)                                   \
    
#define USLEEP(x, ...) do {                     \
    }while(0)                                   \
    

static void bitbang_exchange(bool rnw, uint8_t buf[], unsigned int offset, unsigned int bit_cnt)
{
	LOG_DEBUG("bitbang_exchange");
//	int tdi;
    int data_len = DIV_ROUND_UP(bit_cnt + offset, 8);
    char hex[128];
    int n;
    int i = 0;
    int data_cnt = 0;
    if (rnw) {
        hex[0] = 0xF1;
        hex[1] = GET_UPPER_HEX(bit_cnt);
        hex[2] = GET_LOWER_HEX(bit_cnt);
        hex[3] = GET_UPPER_HEX(offset);
        hex[4] = GET_LOWER_HEX(offset);
        if (buf != NULL) {
            hex[5] = GET_UPPER_HEX(data_len);
            hex[6] = GET_LOWER_HEX(data_len);
        } else {
            hex[5] = '0';
            hex[6] = '0';
        }
        n = write(fd, hex, 7);
        if (buf == NULL) {
            usleep(10000);
            return;
        }
        while(1) {
            n = read (fd, hex, sizeof(hex));  // read up to 1 character if ready to read */
            for (i = 0; i < n; i++) {
                PRINTF("data_cnt = %d\r\n", data_cnt);
                if ((data_cnt & 0x01) == 0) {
                    local_buf[(data_cnt++) / 2] = (HEX_TO_INT(hex[i]) << 4);
                } else {
                    local_buf[(data_cnt++) / 2] |= (HEX_TO_INT(hex[i]) << 0);
                }
            }
            if (data_cnt / 2 == data_len) {
                /* LOG_DEBUG("Read OK\r\n");                  */
                for (i = offset; (unsigned int)i < bit_cnt + offset; i++) {
                    int bytec = i/8;
                    int bcval = 1 << (i % 8);
                    if (local_buf[bytec] & bcval)
                        buf[bytec] |= bcval;
                    else
                        buf[bytec] &= ~bcval;
                }
               PRINTF("read receiving:\r\n");
                for (i = 0; i < data_len; i++) {
                    PRINTF("%02x ", buf[i]);
                }
                PRINTF("\r\n");
                return;
            }
        }
    } else {
        hex[0] = 0xF0;
        hex[1] = GET_UPPER_HEX(bit_cnt);
        hex[2] = GET_LOWER_HEX(bit_cnt);
        hex[3] = GET_UPPER_HEX(offset);
        hex[4] = GET_LOWER_HEX(offset);
        if (buf != NULL) {
            hex[5] = GET_UPPER_HEX(data_len);
            hex[6] = GET_LOWER_HEX(data_len);
        } else {
            hex[5] = '0';
            hex[6] = '0';
        }
        n = write(fd, hex, 7);
        if (buf != NULL) {
            for (i = 0; i < data_len; i++){
                hex[0] = GET_UPPER_HEX(buf[i]);
                hex[1] = GET_LOWER_HEX(buf[i]);
                n = write(fd, hex, 2);                            
            }
        }
        data_cnt = 0;
        while(1) {
            n = read (fd, hex, sizeof(hex));  // read up to 1 character if ready to read */
            for (i = 0; i < n; i++) {
                if ((data_cnt & 0x01) == 0) {
                    local_buf[(data_cnt++) / 2] = (HEX_TO_INT(hex[i]) << 4);
                } else {
                    local_buf[(data_cnt++) / 2] |= (HEX_TO_INT(hex[i]) << 0);
                }
            }
            if (data_cnt / 2 == 1) {
                return;
            }
        }
    }
}

int bitbang_swd_switch_seq(enum swd_special_seq seq)
{
	LOG_DEBUG("bitbang_swd_switch_seq");

	switch (seq) {
	case LINE_RESET:
		LOG_DEBUG("SWD line reset");
		bitbang_exchange(false, (uint8_t *)swd_seq_line_reset, 0, swd_seq_line_reset_len);
		break;
	case JTAG_TO_SWD:
		LOG_DEBUG("JTAG-to-SWD");
		bitbang_exchange(false, (uint8_t *)swd_seq_jtag_to_swd, 0, swd_seq_jtag_to_swd_len);
		break;
	case SWD_TO_JTAG:
		LOG_DEBUG("SWD-to-JTAG");
		bitbang_exchange(false, (uint8_t *)swd_seq_swd_to_jtag, 0, swd_seq_swd_to_jtag_len);
		break;
	default:
		LOG_ERROR("Sequence %d not supported", seq);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

void bitbang_switch_to_swd(void)
{
	LOG_DEBUG("bitbang_switch_to_swd");
	bitbang_exchange(false, (uint8_t *)swd_seq_jtag_to_swd, 0, swd_seq_jtag_to_swd_len);
}

static void swd_clear_sticky_errors(void)
{
	bitbang_swd_write_reg(swd_cmd(false,  false, DP_ABORT),
		STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR, 0);
}
static void bitbang_interface_swdio_drive(bool out)
{
    volatile int i;
    uint8_t hex[2];
    if (out) {
        hex[0] = 0xE1;
        i = write(fd, hex, 1);
    } else {
        hex[0] = 0xE0;
        i = write(fd, hex, 1);
    }
    if (i == 0) {
        LOG_DEBUG("What?\r\n");
    }
    
};
static void bitbang_swd_read_reg(uint8_t cmd, uint32_t *value, uint32_t ap_delay_clk)
{
	LOG_DEBUG("bitbang_swd_read_reg");
	assert(cmd & SWD_CMD_RnW);

	if (queued_retval != ERROR_OK) {
		LOG_DEBUG("Skip bitbang_swd_read_reg because queued_retval=%d", queued_retval);
		return;
	}

	for (;;) {
		uint8_t trn_ack_data_parity_trn[DIV_ROUND_UP(4 + 3 + 32 + 1 + 4, 8)];

		cmd |= SWD_CMD_START | (1 << 7);
		bitbang_exchange(false, &cmd, 0, 8);

		bitbang_interface_swdio_drive(false);
		bitbang_exchange(true, trn_ack_data_parity_trn, 0, 1 + 3 + 32 + 1 + 1);
		bitbang_interface_swdio_drive(true);

		int ack = buf_get_u32(trn_ack_data_parity_trn, 1, 3);
		uint32_t data = buf_get_u32(trn_ack_data_parity_trn, 1 + 3, 32);
		int parity = buf_get_u32(trn_ack_data_parity_trn, 1 + 3 + 32, 1);

		LOG_DEBUG("%s %s %s reg %X = %08"PRIx32,
			  ack == SWD_ACK_OK ? "OK" : ack == SWD_ACK_WAIT ? "WAIT" : ack == SWD_ACK_FAULT ? "FAULT" : "JUNK",
			  cmd & SWD_CMD_APnDP ? "AP" : "DP",
			  cmd & SWD_CMD_RnW ? "read" : "write",
			  (cmd & SWD_CMD_A32) >> 1,
			  data);

		switch (ack) {
		 case SWD_ACK_OK:
			if (parity != parity_u32(data)) {
				LOG_DEBUG("Wrong parity detected");
				queued_retval = ERROR_FAIL;
				return;
			}
			if (value)
				*value = data;
			if (cmd & SWD_CMD_APnDP)
				bitbang_exchange(true, NULL, 0, ap_delay_clk);
			return;
		 case SWD_ACK_WAIT:
			LOG_DEBUG("SWD_ACK_WAIT");
			swd_clear_sticky_errors();
			break;
		 case SWD_ACK_FAULT:
			LOG_DEBUG("SWD_ACK_FAULT");
			queued_retval = ack;
			return;
		 default:
			LOG_DEBUG("No valid acknowledge: ack=%d", ack);
			queued_retval = ack;
			return;
		}
	}
}

static void bitbang_swd_write_reg(uint8_t cmd, uint32_t value, uint32_t ap_delay_clk)
{
	LOG_DEBUG("bitbang_swd_write_reg");
	assert(!(cmd & SWD_CMD_RnW));

	if (queued_retval != ERROR_OK) {
		LOG_DEBUG("Skip bitbang_swd_write_reg because queued_retval=%d", queued_retval);
		return;
	}

	for (;;) {
		uint8_t trn_ack_data_parity_trn[DIV_ROUND_UP(4 + 3 + 32 + 1 + 4, 8)];
		buf_set_u32(trn_ack_data_parity_trn, 1 + 3 + 1, 32, value);
		buf_set_u32(trn_ack_data_parity_trn, 1 + 3 + 1 + 32, 1, parity_u32(value));

		cmd |= SWD_CMD_START | (1 << 7);
		bitbang_exchange(false, &cmd, 0, 8);

		bitbang_interface_swdio_drive(false);
		bitbang_exchange(true, trn_ack_data_parity_trn, 0, 1 + 3 + 1);
		bitbang_interface_swdio_drive(true);
		bitbang_exchange(false, trn_ack_data_parity_trn, 1 + 3 + 1, 32 + 1);

		int ack = buf_get_u32(trn_ack_data_parity_trn, 1, 3);
		LOG_DEBUG("%s %s %s reg %X = %08"PRIx32,
			  ack == SWD_ACK_OK ? "OK" : ack == SWD_ACK_WAIT ? "WAIT" : ack == SWD_ACK_FAULT ? "FAULT" : "JUNK",
			  cmd & SWD_CMD_APnDP ? "AP" : "DP",
			  cmd & SWD_CMD_RnW ? "read" : "write",
			  (cmd & SWD_CMD_A32) >> 1,
			  buf_get_u32(trn_ack_data_parity_trn, 1 + 3 + 1, 32));

		switch (ack) {
		 case SWD_ACK_OK:
			if (cmd & SWD_CMD_APnDP)
				bitbang_exchange(true, NULL, 0, ap_delay_clk);
			return;
		 case SWD_ACK_WAIT:
			LOG_DEBUG("SWD_ACK_WAIT");
			swd_clear_sticky_errors();
			break;
		 case SWD_ACK_FAULT:
			LOG_DEBUG("SWD_ACK_FAULT");
			queued_retval = ack;
			return;
		 default:
			LOG_DEBUG("No valid acknowledge: ack=%d", ack);
			queued_retval = ack;
			return;
		}
	}
}

static int bitbang_swd_run_queue(void)
{
	LOG_DEBUG("bitbang_swd_run_queue");
	/* A transaction must be followed by another transaction or at least 8 idle cycles to
	 * ensure that data is clocked through the AP. */
	bitbang_exchange(true, NULL, 0, 8);

	int retval = queued_retval;
	queued_retval = ERROR_OK;
	LOG_DEBUG("SWD queue return value: %02x", retval);
	return retval;
}

const struct swd_driver bitbang_swd = {
	.init = bitbang_swd_init,
	.switch_seq = bitbang_swd_switch_seq,
	.read_reg = bitbang_swd_read_reg,
	.write_reg = bitbang_swd_write_reg,
	.run = bitbang_swd_run_queue,
};
