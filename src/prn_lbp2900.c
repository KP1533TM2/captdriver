/*
 * Copyright (C) 2013 Alexey Galakhov <agalakhov@gmail.com>
 * Copyright (C) 2016 Alexei Gordeev <KP1533TM2@gmail.com>
 *
 * Licensed under the GNU General Public License Version 3
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "std.h"
#include "word.h"
#include "capt-command.h"
#include "capt-status.h"
#include "generic-ops.h"
#include "hiscoa-common.h"
#include "hiscoa-compress.h"
#include "paper.h"
#include "printer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct lbp2900_ops_s {
	struct printer_ops_s ops;

	const struct capt_status_s * (*get_status) (void);
	void (*wait_ready) (void);
};

static const struct capt_status_s *lbp2900_get_status(const struct printer_ops_s *ops)
{
	const struct lbp2900_ops_s *lops = container_of(ops, struct lbp2900_ops_s, ops);
	return lops->get_status();
}

static void lbp2900_wait_ready(const struct printer_ops_s *ops)
{
	const struct lbp2900_ops_s *lops = container_of(ops, struct lbp2900_ops_s, ops);
	lops->wait_ready();
}

static void send_job_start()
{
	uint16_t page = 1; /* nobody cares */
	uint8_t nl = 16;
	uint8_t fg = 0x01;
	uint16_t job = 1;  /* nobody cares */
	time_t rawtime = time(NULL);
	const struct tm *tm = localtime(&rawtime);
	uint8_t buf[32 + 64 + nl];
	uint8_t head[32] = {
		0x00, 0x00, 0x00, 0x00, LO(page), HI(page), 0x00, 0x00,
		0x10, 0x00, 0x0C, 0x00, nl, 0x00, 0x00, 0x00,
		fg, 0x01, LO(job), HI(job), /*-60*/ 0xC4, 0xFF, /*-120*/ 0x88, 0xFF,
		LO(tm->tm_year), HI(tm->tm_year), (uint8_t) tm->tm_mon, (uint8_t) tm->tm_mday,
		(uint8_t) tm->tm_hour, (uint8_t) tm->tm_min, (uint8_t) tm->tm_sec,
		0x01,
	};
	memcpy(buf, head, sizeof(head));
	memset(buf + 32, 0, 64 + nl);
	capt_sendrecv(CAPT_JOB_SETUP, buf, sizeof(buf), NULL, 0);
}

static const uint8_t magicbuf_0[] = {
	0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t magicbuf_2[] = {
	0xEE, 0xDB, 0xEA, 0xAD, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void lbp2900_job_prologue(struct printer_state_s *state)
{
	(void) state;
	capt_sendrecv(CAPT_IDENT, NULL, 0, NULL, 0);
	sleep(1);
	capt_init_status();
	lbp2900_get_status(state->ops);

	capt_sendrecv(CAPT_START_0, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_JOB_BEGIN, magicbuf_0, ARRAY_SIZE(magicbuf_0), NULL, 0);
	lbp2900_wait_ready(state->ops);
	send_job_start();
	lbp2900_wait_ready(state->ops);
}

static void lbp3000_job_prologue(struct printer_state_s *state)
{
	(void) state;
	capt_sendrecv(CAPT_IDENT, NULL, 0, NULL, 0);
	sleep(1);
	capt_init_status();
	lbp2900_get_status(state->ops);

	capt_sendrecv(CAPT_START_0, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_JOB_BEGIN, magicbuf_0, ARRAY_SIZE(magicbuf_0), NULL, 0);
	/* LBP-3000 prints the very first printjob perfectly
	 * and then proceeds to hang at this (commented out)
	 * spot. That's the difference, or so it seems. */
/*	lbp2900_wait_ready(state->ops);	*/
	send_job_start();
	
	/* There's also that command, that apparently does something, and does something, 
	 * but it's there in the Wireshark logs. Response data == command data. */
	uint8_t dummy[2] = {0,0};
	capt_sendrecv(0xE0A6, dummy, sizeof(dummy), NULL, 0);
	
	lbp2900_wait_ready(state->ops);
}

static bool lbp2900_page_prologue(struct printer_state_s *state, const struct page_dims_s *dims)
{
	const struct capt_status_s *status;
	size_t s;
	uint8_t buf[16];

	uint8_t pt1 = 0x00;
	uint8_t save = 0x00;
	uint8_t pt2 = 0x01;

	uint8_t pageparms[] = {
		0x00, 0x00, 0x30, 0x2A, /* sz */ 0x02, 0x00, 0x00, 0x00,
		0x1F, 0x1F, 0x1F, 0x1F, pt1, 0x11, 0x04, 0x00,
		0x01, 0x01, 0x02, save, 0x00, 0x00, /*120*/ 0x78, 0x00,
		/*96*/ 0x60, 0x00,
		LO(dims->line_size), HI(dims->line_size), LO(dims->num_lines), HI(dims->num_lines),
		LO(dims->paper_width), HI(dims->paper_width),
		LO(dims->paper_height), HI(dims->paper_height),
		/* 34 bytes */
		0x00, 0x00, pt2, 0x00, 0x00, 0x00,
	};

	(void) state;

	status = lbp2900_get_status(state->ops);
	if (FLAG(status, CAPT_FL_UNINIT1) || FLAG(status, CAPT_FL_UNINIT2)) {
		capt_sendrecv(CAPT_START_1, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_START_2, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_START_3, NULL, 0, NULL, 0);
		lbp2900_wait_ready(state->ops);
		capt_sendrecv(CAPT_UPLOAD_2, magicbuf_2, ARRAY_SIZE(magicbuf_2), NULL, 0);
		lbp2900_wait_ready(state->ops);
	}

	while (1) {
		if (! FLAG(lbp2900_get_status(state->ops), CAPT_FL_BUFFERFULL))
			break;
		sleep(1);
	}

	capt_multi_begin(CAPT_SET_PARMS);
	capt_multi_add(CAPT_SET_PARM_PAGE, pageparms, sizeof(pageparms));
	s = hiscoa_format_params(buf, sizeof(buf), &hiscoa_default_params);
	capt_multi_add(CAPT_SET_PARM_HISCOA, buf, s);
	capt_multi_add(CAPT_SET_PARM_1, NULL, 0);
	capt_multi_add(CAPT_SET_PARM_2, NULL, 0);
	capt_multi_send();

	return true;
}

static bool lbp2900_page_epilogue(struct printer_state_s *state, const struct page_dims_s *dims)
{
	uint8_t buf[2] = { LO(state->ipage), HI(state->ipage) };
	(void) dims;
	capt_send(CAPT_PRINT_DATA_END, NULL, 0);
	capt_sendrecv(CAPT_FIRE, buf, 2, NULL, 0);
	lbp2900_wait_ready(state->ops);

	while (1) {
		const struct capt_status_s *status = lbp2900_get_status(state->ops);
		/* Interesting. Using page_printing here results in shifted print */
		if (status->page_out >= state->ipage)
			return true;
		if (FLAG(status, CAPT_FL_NOPAPER2)) {
			fprintf(stderr, "DEBUG: CAPT: no paper\n");
			if (FLAG(status, CAPT_FL_PRINTING) || FLAG(status, CAPT_FL_PROCESSING1))
				continue;
			return false;
		}
		sleep(1);
	}
}

static void lbp2900_job_epilogue(struct printer_state_s *state)
{
	uint8_t buf[2] = { LO(state->ipage), HI(state->ipage) };
	while (1) {
		if (lbp2900_get_status(state->ops)->page_completed >= state->ipage)
			break;
		sleep(1);
	}
	capt_sendrecv(CAPT_JOB_END, buf, 2, NULL, 0);
}

static void lbp2900_page_setup(struct printer_state_s *state,
		struct page_dims_s *dims,
		unsigned width, unsigned height)
{
/*
	A4		4736x6784
	Letter		4864x6368
	Legal		4864x8192
	Executive	4128x6080
	3x5		1344x2528
*/
	(void) state;
	(void) width;
	dims->band_size = 70;
	dims->line_size = 4736 / 8;
	if (height > 6784)
		dims->num_lines = 6784;
	else
		dims->num_lines = height;
}

static void lbp2900_wait_user(struct printer_state_s *state)
{
	(void) state;
	while (1) {
		const struct capt_status_s *status = lbp2900_get_status(state->ops);
		if (FLAG(status, CAPT_FL_BUTTON)) {
			fprintf(stderr, "DEBUG: CAPT: button pressed\n");
			break;
		}
		sleep(1);
	}
}

static struct lbp2900_ops_s lbp2900_ops = {
	.ops = {
		.job_prologue = lbp2900_job_prologue,
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.wait_user = lbp2900_wait_user,
	},
	.get_status = capt_get_xstatus,
	.wait_ready = capt_wait_ready,
};

static struct lbp2900_ops_s lbp3000_ops = {
	.ops = {
		.job_prologue = lbp3000_job_prologue,	/* different job prologue */
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.wait_user = lbp2900_wait_user,
	},
	.get_status = capt_get_xstatus,
	.wait_ready = capt_wait_ready,
};

register_printer("LBP2900", lbp2900_ops.ops, WORKS);
register_printer("LBP3000", lbp3000_ops.ops, EXPERIMENTAL);

static struct lbp2900_ops_s lbp3010_ops = {
	.ops = {
		.job_prologue = lbp2900_job_prologue,
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.wait_user = lbp2900_wait_user,
	},
	.get_status = capt_get_xstatus_only,
	.wait_ready = capt_wait_xready,
};

register_printer("LBP3010/LBP3018/LBP3050", lbp3010_ops.ops, EXPERIMENTAL);
