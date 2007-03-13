/*****************************************************************************\
 *  trigger_mgr.c - Event trigger management
 *****************************************************************************
 *  Copyright (C) 2007 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov> et. al.
 *  UCRL-CODE-226842.
 *  
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.llnl.gov/linux/slurm/>.
 *  
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission 
 *  to link the code of portions of this program with the OpenSSL library under 
 *  certain conditions as described in each individual source file, and 
 *  distribute linked combinations including the two. You must obey the GNU 
 *  General Public License in all respects for all of the code used other than 
 *  OpenSSL. If you modify file(s) with this exception, you may extend this 
 *  exception to your version of the file(s), but you are not obligated to do 
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in 
 *  the program, then also delete it here.
 *  
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef WITH_PTHREADS
#  include <pthread.h>
#endif

#include <errno.h>
#include <stdlib.h>

#include "src/common/bitstring.h"
#include "src/common/list.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/trigger_mgr.h"

#define _DEBUG 1
#define MAX_PROG_TIME 300	/* maximum run time for program */

List trigger_list;
uint32_t next_trigger_id = 1;
static pthread_mutex_t trigger_mutex = PTHREAD_MUTEX_INITIALIZER;
bitstr_t *trigger_down_nodes_bitmap = NULL;
bitstr_t *trigger_up_nodes_bitmap   = NULL;

typedef struct trig_mgr_info {
	uint32_t trig_id;	/* trigger ID */
	uint8_t  res_type;	/* TRIGGER_RES_TYPE_* */
	char *   res_id;	/* node name or job_id (string) */
	bitstr_t *nodes_bitmap;	/* bitmap of requested nodes (if applicable) */
	uint32_t job_id;	/* job ID (if applicable) */
	struct job_record *job_ptr; /* pointer to job record (if applicable) */
	uint8_t  trig_type;	/* TRIGGER_TYPE_* */
	time_t   trig_time;	/* offset (pending) or time stamp (complete) */
	uint32_t user_id;	/* user requesting trigger */
	uint32_t group_id;	/* user's group id (pending) or pid (complete) */
	char *   program;	/* program to execute */
	uint8_t  state;		/* 0=pending, 1=pulled, 2=completed */
} trig_mgr_info_t;

/* Prototype for ListDelF */
void _trig_del(void *x) {
	trig_mgr_info_t * tmp = (trig_mgr_info_t *) x;
	xfree(tmp->res_id);
	xfree(tmp->program);
	FREE_NULL_BITMAP(tmp->nodes_bitmap);
	xfree(tmp);
}

static char *_res_type(uint8_t  res_type)
{
	if      (res_type == TRIGGER_RES_TYPE_JOB)
		return "job";
	else if (res_type == TRIGGER_RES_TYPE_NODE)
		return "node";
	else
		return "unknown";
}

static char *_trig_type(uint8_t  trig_type)
{
	if      (trig_type == TRIGGER_TYPE_UP)
		return "up";
	else if (trig_type == TRIGGER_TYPE_DOWN)
		return "down";
	else if (trig_type == TRIGGER_TYPE_TIME)
		return "time";
	else if (trig_type == TRIGGER_TYPE_FINI)
		return "fini";
	else
		return "unknown";
}

static int _trig_offset(uint16_t offset)
{
	static int rc;
	rc  = offset;
	rc -= 0x8000;
	return rc;
}

static void _dump_trigger_msg(char *header, trigger_info_msg_t *msg)
{
#if _DEBUG
	int i;

	info(header);
	if ((msg == NULL) || (msg->record_count == 0)) {
		info("Trigger has no entries");
		return;
	}

	info("INDEX TRIG_ID RES_TYPE RES_ID TRIG_TYPE OFFSET UID PROGRAM");
	for (i=0; i<msg->record_count; i++) {
		info("trigger[%u] %u %s %s %s %d %u %s", i,
			msg->trigger_array[i].trig_id,
			_res_type(msg->trigger_array[i].res_type),
			msg->trigger_array[i].res_id,
			_trig_type(msg->trigger_array[i].trig_type),
			_trig_offset(msg->trigger_array[i].offset),
			msg->trigger_array[i].user_id,
			msg->trigger_array[i].program);
	}
#endif
}

extern int trigger_clear(uid_t uid, trigger_info_msg_t *msg)
{
	int rc = ESRCH;
	ListIterator trig_iter;
	trigger_info_t *trig_in;
	trig_mgr_info_t *trig_test;
	uint32_t job_id = 0;

	slurm_mutex_lock(&trigger_mutex);
	if (trigger_list == NULL)
		trigger_list = list_create(_trig_del);

	/* validate the request, need a job_id and/or trigger_id */
	_dump_trigger_msg("trigger_clear", msg);
	if (msg->record_count != 1)
		goto fini;
	trig_in = msg->trigger_array;
	if (trig_in->res_type == TRIGGER_RES_TYPE_JOB) {
		job_id = (uint32_t) atol(trig_in->res_id);
		if (job_id == 0) {
			rc = ESLURM_INVALID_JOB_ID;
			goto fini;
		}
	} else if (trig_in->trig_id == 0) {
		rc = EINVAL;
		goto fini;
	}

	/* now look for a valid request, matching uid */
	trig_iter = list_iterator_create(trigger_list);
	while ((trig_test = list_next(trig_iter))) {
		if ((trig_test->user_id != (uint32_t) uid)
		&&  (uid != 0))
			continue;
		if (trig_in->trig_id
		&&  (trig_in->trig_id != trig_test->trig_id))
			continue;
		if (job_id
		&& (job_id != trig_test->job_id))
			continue;
		list_delete(trig_iter);
		rc = SLURM_SUCCESS;
	}
	list_iterator_destroy(trig_iter);

fini:	slurm_mutex_unlock(&trigger_mutex);
	return rc;
}

extern trigger_info_msg_t * trigger_get(uid_t uid, trigger_info_msg_t *msg)
{
	trigger_info_msg_t *resp_data;
	ListIterator trig_iter;
	trigger_info_t *trig_out;
	trig_mgr_info_t *trig_in;
	int recs_written = 0;

	slurm_mutex_lock(&trigger_mutex);
	if (trigger_list == NULL)
		trigger_list = list_create(_trig_del);

	_dump_trigger_msg("trigger_get", NULL);
	resp_data = xmalloc(sizeof(trigger_info_msg_t));
	resp_data->record_count = list_count(trigger_list);
	resp_data->trigger_array = xmalloc(sizeof(trigger_info_t) *
			resp_data->record_count);
	trig_iter = list_iterator_create(trigger_list);
	trig_out = resp_data->trigger_array;
	while ((trig_in = list_next(trig_iter))) {
		/* Note: Filtering currently done by strigger */
		if (trig_in->state > 1)
			continue;	/* no longer pending */
		trig_out->trig_id   = trig_in->trig_id;
		trig_out->res_type  = trig_in->res_type;
		trig_out->res_id    = xstrdup(trig_in->res_id);
		trig_out->trig_type = trig_in->trig_type;
		trig_out->offset    = trig_in->trig_time;
		trig_out->user_id   = trig_in->user_id;
		trig_out->program   = xstrdup(trig_in->program);
		trig_out++;
		recs_written++;
	}
	list_iterator_destroy(trig_iter);
	slurm_mutex_unlock(&trigger_mutex);
	resp_data->record_count = recs_written;

	_dump_trigger_msg("trigger_got", resp_data);
	return resp_data;
}

extern int trigger_set(uid_t uid, gid_t gid, trigger_info_msg_t *msg)
{
	int i;
	int rc = SLURM_SUCCESS;
	uint32_t job_id;
	bitstr_t *bitmap = NULL;
	trig_mgr_info_t * trig_add;
	struct job_record *job_ptr;
	slurmctld_lock_t job_read_lock =
		{ NO_LOCK, READ_LOCK, NO_LOCK, NO_LOCK };

	lock_slurmctld(job_read_lock);
	slurm_mutex_lock(&trigger_mutex);
	if (trigger_list == NULL) {
		trigger_list = list_create(_trig_del);
	} else if ((uid != 0) &&
	           (list_count(trigger_list) >= slurmctld_conf.max_job_cnt)) {
		rc = EAGAIN;
		goto fini;
	}

	_dump_trigger_msg("trigger_set", msg);
	for (i=0; i<msg->record_count; i++) {
		if (msg->trigger_array[i].res_type ==
				TRIGGER_RES_TYPE_JOB) {

			job_id = (uint32_t) atol(
				msg->trigger_array[i].res_id);
			job_ptr = find_job_record(job_id);
			if (job_ptr == NULL) {
				rc = ESLURM_INVALID_JOB_ID;
				continue;
			}
			if (IS_JOB_FINISHED(job_ptr)) {
				rc = ESLURM_ALREADY_DONE;
				continue;
			}
		} else {
			job_id = 0;
			job_ptr = NULL;
			if ((msg->trigger_array[i].res_id != NULL)
			&&  (msg->trigger_array[i].res_id[0] != '*')
			&&  (node_name2bitmap(msg->trigger_array[i].res_id,
						false, &bitmap) != 0)) {
				rc = ESLURM_INVALID_NODE_NAME;
				continue;
			}
		}
		trig_add = xmalloc(sizeof(trig_mgr_info_t));
		msg->trigger_array[i].trig_id = next_trigger_id;
		trig_add->trig_id = next_trigger_id;
		next_trigger_id++;
		trig_add->res_type = msg->trigger_array[i].res_type;
		trig_add->nodes_bitmap = bitmap;
		trig_add->job_id = job_id;
		trig_add->job_ptr = job_ptr;
		/* move don't copy "res_id" */
		trig_add->res_id = msg->trigger_array[i].res_id;
		msg->trigger_array[i].res_id = NULL;
		trig_add->trig_type = msg->trigger_array[i].trig_type;
		trig_add->trig_time = msg->trigger_array[i].offset;
		trig_add->user_id = (uint32_t) uid;
		trig_add->group_id = (uint32_t) gid;
		/* move don't copy "program" */
		trig_add->program = msg->trigger_array[i].program;
		msg->trigger_array[i].program = NULL;
		list_append(trigger_list, trig_add);
	}

fini:	slurm_mutex_unlock(&trigger_mutex);
	unlock_slurmctld(job_read_lock);
	return rc;
}

extern void trigger_node_down(struct node_record *node_ptr)
{
        int inx = node_ptr - node_record_table_ptr;

	if (trigger_down_nodes_bitmap == NULL)
		trigger_down_nodes_bitmap = bit_alloc(node_record_count);
	bit_set(trigger_down_nodes_bitmap, inx);
}

extern void trigger_node_up(struct node_record *node_ptr)
{
        int inx = node_ptr - node_record_table_ptr;

	if (trigger_up_nodes_bitmap == NULL)
		trigger_up_nodes_bitmap = bit_alloc(node_record_count);
	bit_set(trigger_up_nodes_bitmap, inx);
}

extern void trigger_state_save(void)
{
	/* FIXME */
}

extern void trigger_state_restore(void)
{
	/* FIXME */
}

/* Test if the event has been triggered, change trigger state as needed */
static void _trigger_job_event(trig_mgr_info_t *trig_in, time_t now)
{
	if ((trig_in->job_ptr == NULL)
	||  (trig_in->job_ptr->job_id == trig_in->job_id))
		trig_in->job_ptr = find_job_record(trig_in->job_ptr->job_id);
	if ((trig_in->trig_type & TRIGGER_TYPE_FINI)
	&&  ((trig_in->job_ptr == NULL) ||
	     (IS_JOB_FINISHED(trig_in->job_ptr)))) {
#if _DEBUG
		info("trigger[%u] event for job %u fini",
			trig_in->trig_id, trig_in->job_id);
#endif
		trig_in->state = 1;
		return;
	}
	if (trig_in->job_ptr == NULL) {
#if _DEBUG
		info("trigger[%u] for defunct job %u",
			trig_in->trig_id, trig_in->job_id);
#endif
		trig_in->state = 2;
		trig_in->trig_time = now;
		return;
	}
	if (trig_in->trig_type & TRIGGER_TYPE_TIME) {
		long rem_time = (trig_in->job_ptr->end_time - now);
		if (rem_time <= (0x8000 - trig_in->trig_time)) {
#if _DEBUG
			info("trigger[%u] for job %u time",
				trig_in->trig_id, trig_in->job_id);
#endif
			trig_in->state = 1;
			return;
		}
	}
	if (trig_in->trig_type & TRIGGER_TYPE_DOWN) {
		if (trigger_down_nodes_bitmap
		&&  bit_overlap(trig_in->job_ptr->node_bitmap, 
				trigger_down_nodes_bitmap)) {
#if _DEBUG
			info("trigger[%u] for job %u down",
				trig_in->trig_id, trig_in->job_id);
#endif
			trig_in->state = 1;
			return;
		}
	}
	if (trig_in->trig_type & TRIGGER_TYPE_UP) {
		if (trigger_down_nodes_bitmap
		&&  bit_overlap(trig_in->job_ptr->node_bitmap, 
				trigger_up_nodes_bitmap)) {
#if _DEBUG
			info("trigger[%u] for job %u up",
				trig_in->trig_id, trig_in->job_id);
#endif
			trig_in->state = 1;
			return;
		}
	}
}

static void _trigger_node_event(trig_mgr_info_t *trig_in, time_t now)
{
	if ((trig_in->trig_type & TRIGGER_TYPE_DOWN)
	&&   trigger_down_nodes_bitmap
	&&   (bit_ffs(trigger_down_nodes_bitmap) != -1)) {
		if (trig_in->nodes_bitmap == NULL) {	/* all nodes */
			trig_in->res_id = bitmap2node_name(
					trigger_down_nodes_bitmap);
			trig_in->state = 1;
		} else if (bit_overlap(trig_in->nodes_bitmap, 
					trigger_down_nodes_bitmap)) {
			bit_and(trig_in->nodes_bitmap, 
					trigger_down_nodes_bitmap);
			xfree(trig_in->res_id);
			trig_in->res_id = bitmap2node_name(
					trig_in->nodes_bitmap);
			trig_in->state = 1;
		}
		if (trig_in->state == 1) {
#if _DEBUG
			info("trigger[%u] for node %s down",
				trig_in->trig_id, trig_in->res_id);
#endif
			return;
		}
	}
	if ((trig_in->trig_type & TRIGGER_TYPE_UP)
	&&   trigger_up_nodes_bitmap
	&&   (bit_ffs(trigger_up_nodes_bitmap) != -1)) {
		if (trig_in->nodes_bitmap == NULL) {	/* all nodes */
			trig_in->res_id = bitmap2node_name(
					trigger_up_nodes_bitmap);
			trig_in->state = 1;
		} else if (bit_overlap(trig_in->nodes_bitmap, 
					trigger_up_nodes_bitmap)) {
			bit_and(trig_in->nodes_bitmap, 
					trigger_up_nodes_bitmap);
			xfree(trig_in->res_id);
			trig_in->res_id = bitmap2node_name(
					trig_in->nodes_bitmap);
			trig_in->state = 1;
		}
		if (trig_in->state == 1) {
#if _DEBUG
			info("trigger[%u] for node %s up",
				trig_in->trig_id, trig_in->res_id);
#endif
			return;
		}
	}
}

static void _trigger_run_program(trig_mgr_info_t *trig_in)
{
	char program[256], arg0[128], arg1[128], *pname;
	uid_t uid;
	gid_t gid;
	pid_t child;

	strncpy(program, trig_in->program, sizeof(program));
	pname = strrchr(program, '/');
	if (pname == NULL)
		pname = program;
	else
		pname++;
	strncpy(arg0, pname, sizeof(arg0));
/* FIXME: set to actual node list below */
	strncpy(arg1, trig_in->res_id, sizeof(arg1));
	uid = trig_in->user_id;
	gid = trig_in->group_id;
	child = fork();
	if (child > 0) {
		trig_in->group_id = child;
	} else if (child == 0) {
		int i;
		for (i=0; i<128; i++)
			close(i);
		setpgrp();
		setsid();
		setuid(uid);
		setgid(gid);
		execl(program, arg0, arg1, NULL);
		exit(1);
	} else
		error("fork: %m");
}

extern void trigger_process(void)
{
	ListIterator trig_iter;
	trig_mgr_info_t *trig_in;
	time_t now = time(NULL);
	slurmctld_lock_t job_node_read_lock =
		{ NO_LOCK, READ_LOCK, READ_LOCK, NO_LOCK };

	lock_slurmctld(job_node_read_lock);
	slurm_mutex_lock(&trigger_mutex);
	if (trigger_list == NULL)
		trigger_list = list_create(_trig_del);

	trig_iter = list_iterator_create(trigger_list);
	while ((trig_in = list_next(trig_iter))) {
		if (trig_in->state == 0) {
			if (trig_in->res_type == TRIGGER_RES_TYPE_JOB)
				_trigger_job_event(trig_in, now);
			else
				_trigger_node_event(trig_in, now);
		}
		if (trig_in->state == 1) {
#if _DEBUG
			info("launching program for trigger[%u]",
				trig_in->trig_id);
#endif
			trig_in->state = 2;
			trig_in->trig_time = now;
			_trigger_run_program(trig_in);
		} else if ((trig_in->state == 2) && 
			   (difftime(now, trig_in->trig_time) > 
					MAX_PROG_TIME)) {
#if _DEBUG
			info("purging trigger[%u]", trig_in->trig_id);
#endif
			if (trig_in->group_id != 0)
				kill(-trig_in->group_id, SIGKILL);
			list_delete(trig_iter);
		}
	}
	list_iterator_destroy(trig_iter);
	if (trigger_down_nodes_bitmap)
		bit_nclear(trigger_down_nodes_bitmap, 0, (node_record_count-1));
	if (trigger_up_nodes_bitmap)
		bit_nclear(trigger_up_nodes_bitmap,   0, (node_record_count-1));
	slurm_mutex_unlock(&trigger_mutex);
	unlock_slurmctld(job_node_read_lock);
}
