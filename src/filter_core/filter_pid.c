	/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2017-2018
 *					All rights reserved
 *
 *  This file is part of GPAC / filters sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terfsess of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "filter_session.h"
#include <gpac/constants.h>

void pcki_del(GF_FilterPacketInstance *pcki)
{
	if (safe_int_dec(&pcki->pck->reference_count) == 0) {
		gf_filter_packet_destroy(pcki->pck);
	}
	gf_free(pcki);
}

void gf_filter_pid_inst_reset(GF_FilterPidInst *pidinst)
{
	assert(pidinst);
	while (gf_fq_count(pidinst->packets)) {
		GF_FilterPacketInstance *pcki = gf_fq_pop(pidinst->packets);
		pcki_del(pcki);
	}

	while (gf_list_count(pidinst->pck_reassembly)) {
		GF_FilterPacketInstance *pcki = gf_list_pop_back(pidinst->pck_reassembly);
		pcki_del(pcki);
	}
}

void gf_filter_pid_inst_del(GF_FilterPidInst *pidinst)
{
	assert(pidinst);
	gf_filter_pid_inst_reset(pidinst);

	gf_fq_del(pidinst->packets, (gf_destruct_fun) pcki_del);
	gf_mx_del(pidinst->pck_mx);
	gf_list_del(pidinst->pck_reassembly);
	if (pidinst->props && safe_int_dec(&pidinst->props->reference_count) == 0)
		gf_props_del(pidinst->props);
	gf_free(pidinst);
}

static GF_FilterPidInst *gf_filter_pid_inst_new(GF_Filter *filter, GF_FilterPid *pid)
{
	GF_FilterPidInst *pidinst;
	GF_SAFEALLOC(pidinst, GF_FilterPidInst);
	pidinst->pid = pid;
	pidinst->filter = filter;

	if (filter->session->use_locks) {
		char szName[200];
		u32 pid_idx = 1 + gf_list_find(pid->filter->output_pids, pid);
		u32 dst_idx = 1 + pid->num_destinations;
		snprintf(szName, 200, "F%sPid%dDest%dPackets", filter->name, pid_idx, dst_idx);
		pidinst->pck_mx = gf_mx_new(szName);
	}

	pidinst->packets = gf_fq_new(pidinst->pck_mx);

	pidinst->pck_reassembly = gf_list_new();
	pidinst->last_block_ended = GF_TRUE;
	return pidinst;
}

static void gf_filter_pid_check_unblock(GF_FilterPid *pid)
{
	Bool unblock=GF_FALSE;

	assert(pid->playback_speed_scaler);

	gf_mx_p(pid->filter->tasks_mx);

	//we block according to the number of dispatched units (decoder output) or to the requested buffer duration
	//for other streams - unblock accordingly
	if (pid->max_buffer_unit) {
		if (pid->nb_buffer_unit * GF_FILTER_SPEED_SCALER < pid->max_buffer_unit * pid->playback_speed_scaler) {
			unblock=GF_TRUE;
		}
	} else if (pid->buffer_duration * GF_FILTER_SPEED_SCALER < pid->max_buffer_time * pid->playback_speed_scaler) {
		unblock=GF_TRUE;
	}

	if (pid->would_block && unblock) {
		assert(pid->would_block);
		//todo needs Compare&Swap
		safe_int_dec(&pid->would_block);
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s PID %s unblocked\n", pid->pid->filter->name, pid->pid->name));
		assert(pid->filter->would_block);
		safe_int_dec(&pid->filter->would_block);
		assert((s32)pid->filter->would_block>=0);

		assert(pid->filter->would_block <= pid->filter->num_output_pids);

		if (pid->filter->would_block + pid->filter->num_out_pids_not_connected + pid->filter->num_out_pids_eos < pid->filter->num_output_pids) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s has only %d / %d blocked pids, requesting process task (%d queued)\n", pid->filter->name, pid->filter->would_block, pid->filter->num_output_pids, pid->filter->process_task_queued));
			//requeue task
			gf_filter_post_process_task(pid->filter);
		}

	}
	gf_mx_v(pid->filter->tasks_mx);
}

static void gf_filter_pid_inst_check_dependencies(GF_FilterPidInst *pidi)
{
	const GF_PropertyValue *p;
	u32 i, dep_id = 0;
	GF_FilterPid *pid = pidi->pid;
	GF_Filter *filter = pid->filter;

	//check pid dependency
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_DEPENDENCY_ID);
	if (p) dep_id = p->value.uint;

	if (!dep_id) return;

	for (i=0; i<filter->num_output_pids; i++) {
		u32 j;
		GF_FilterPid *a_pid = gf_list_get(filter->output_pids, i);
		if (a_pid==pid) continue;
		p = gf_filter_pid_get_property(a_pid, GF_PROP_PID_ID);
		if (!p) p = gf_filter_pid_get_property(a_pid, GF_PROP_PID_ESID);
		if (!p || (p->value.uint != dep_id)) continue;

		for (j=0; j<a_pid->num_destinations; j++) {
			GF_FilterPidInst *a_pidi = gf_list_get(a_pid->destinations, j);
			if (a_pidi == pidi) continue;
			if (! a_pidi->is_decoder_input) continue;

			if (a_pidi->filter == pidi->filter) continue;

			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Filter %s PID %s connected to decoder %s, but dependent stream %s connected to %s - switching pid destination\n", a_pid->filter->name, a_pid->name, a_pidi->filter->name, pidi->pid->name, pidi->filter->name));

			//disconnect this pid instance from its current decoder
			gf_fs_post_task(filter->session, gf_filter_pid_disconnect_task, a_pidi->filter, a_pid, "pidinst_disconnect", NULL);

			//reconnect this pid instance to the new decoder
			safe_int_inc(&pid->filter->out_pid_connection_pending);
			gf_filter_pid_post_connect_task(pidi->filter, a_pid);

		}
	}
}

static void gf_filter_pid_update_caps(GF_FilterPid *pid)
{
	u32 mtype=0, codecid=0;
	u32 i, count;
	const GF_PropertyValue *p;

	pid->raw_media = GF_FALSE;
	p = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	if (p) codecid = p->value.uint;

	//by default all buffers are 1ms max
	pid->max_buffer_time = pid->filter->session->default_pid_buffer_max_us;
	pid->max_buffer_unit = pid->filter->session->default_pid_buffer_max_units;
	pid->raw_media = GF_FALSE;

	if (codecid!=GF_CODECID_RAW)
		return;

	if (pid->user_max_buffer_time) {
		pid->max_buffer_time = pid->user_max_buffer_time;
		pid->max_buffer_unit = 0;
	}

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_STREAM_TYPE);
	if (p) mtype = p->value.uint;

	//output is a decoded raw stream: if some input has same type but different codecid this is a decoder
	//set input buffer size
	count=pid->filter->num_input_pids;
	for (i=0; i<count; i++) {
		u32 i_codecid=0, i_type=0;
		GF_FilterPidInst *pidi = gf_list_get(pid->filter->input_pids, i);

		p = gf_filter_pid_get_property(pidi->pid, GF_PROP_PID_STREAM_TYPE);
		if (p) i_type = p->value.uint;

		p = gf_filter_pid_get_property(pidi->pid, GF_PROP_PID_CODECID);
		if (p) i_codecid = p->value.uint;

		//same stream type but changing format type: this is a decoder input pid, set buffer req
		if ((mtype==i_type) && (codecid != i_codecid)) {
			//default decoder buffer
			if (pidi->pid->user_max_buffer_time)
				pidi->pid->max_buffer_time = pidi->pid->user_max_buffer_time;
			else
				pidi->pid->max_buffer_time = pidi->pid->filter->session->decoder_pid_buffer_max_us;
			pidi->pid->max_buffer_unit = 0;


			if (mtype==GF_STREAM_VISUAL) {
				pid->max_buffer_unit = 4;
			} else if (mtype==GF_STREAM_AUDIO) {
				pid->max_buffer_unit = 20;
			}

			if (!pidi->is_decoder_input) {
				GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s pid instance %s marked as decoder input\n",  pidi->pid->filter->name, pidi->pid->name));
				pidi->is_decoder_input = GF_TRUE;
				safe_int_inc(&pidi->pid->nb_decoder_inputs);

				if ((i_type == GF_STREAM_AUDIO) || (i_type == GF_STREAM_VISUAL))
					gf_filter_pid_inst_check_dependencies(pidi);
			}
		}
		//same media type, different codec if is raw stream
		 else if (mtype==i_type) {
			pid->raw_media = GF_TRUE;
		}
	}
	//source pid, mark raw media
	if (!count && pid->num_destinations) {
		pid->raw_media = GF_TRUE;
	}
}

#define TASK_REQUEUE(_t) \
	_t->requeue_request = GF_TRUE; \
	_t->schedule_next_time = gf_sys_clock_high_res() + 50; \

void gf_filter_pid_inst_delete_task(GF_FSTask *task)
{
	GF_FilterPid *pid = task->pid;
	GF_FilterPidInst *pidinst = task->udta;
	GF_Filter *filter = pid->filter;

	//reset in process
	if ((pidinst->filter && pidinst->discard_packets) || filter->stream_reset_pending) {
		TASK_REQUEUE(task)
		return;
	}

	//reset PID instance buffers before checking number of output shared packets
	//otherwise we may block because some of the shared packets are in the
	//pid instance buffer (not consumed)
	gf_filter_pid_inst_reset(pidinst);

	//we still have packets out there!
	if (pidinst->pid->nb_shared_packets_out) {
		TASK_REQUEUE(task)
		return;
	}

	//WARNING at this point pidinst->filter may be destroyed
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s pid instance %s destruction\n",  filter->name, pid->name));
	gf_mx_p(filter->tasks_mx);
	gf_list_del_item(pid->destinations, pidinst);
	pid->num_destinations = gf_list_count(pid->destinations);
	gf_mx_v(filter->tasks_mx);

	if (pidinst->is_decoder_input) {
		assert(pid->nb_decoder_inputs);
		safe_int_dec(&pid->nb_decoder_inputs);
	}
	gf_filter_pid_inst_del(pidinst);
	//recompute max buf dur and nb units to update blocking state
	if (pid->num_destinations) {
		u32 i;
		u32 nb_pck = 0;
		s64 buf_dur = 0;
		for (i = 0; i < pid->num_destinations; i++) {
			GF_FilterPidInst *apidi = gf_list_get(pid->destinations, i);
			u32 npck = gf_fq_count(apidi->packets);
			if (npck > nb_pck) nb_pck = npck;
			if (apidi->buffer_duration > buf_dur) buf_dur = apidi->buffer_duration;
		}
		pid->nb_buffer_unit = nb_pck;
		pid->buffer_duration = buf_dur;
	} else {
		pid->nb_buffer_unit = 0;
		pid->buffer_duration = 0;
	}

	//update blocking state
	if (pid->would_block)
		gf_filter_pid_check_unblock(pid);
	else
		gf_filter_pid_would_block(pid);

	//if filter still has input pids, another filter is still connected to it so we cannot destroy the pid
	if (gf_list_count(filter->input_pids)) {
		return;
	}
	//no more pids on filter, destroy it
	if (! pid->num_destinations ) {
		gf_list_del_item(filter->output_pids, pid);
		filter->num_output_pids = gf_list_count(filter->output_pids);
		gf_filter_pid_del(pid);
	}
	if (!gf_list_count(filter->output_pids) && !gf_list_count(filter->input_pids)) {
		assert(!filter->finalized);
		filter->finalized = GF_TRUE;
		gf_fs_post_task(filter->session, gf_filter_remove_task, filter, NULL, "filter_destroy", NULL);
	}
}

void gf_filter_pid_inst_swap_delete(GF_Filter *filter, GF_FilterPid *pid, GF_FilterPidInst *pidinst, GF_FilterPidInst *dst_swapinst)
{
	u32 i, j;

	//reset PID instance buffers before checking number of output shared packets
	//otherwise we may block because some of the shared packets are in the
	//pid instance buffer (not consumed)
	gf_filter_pid_inst_reset(pidinst);

	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s pid instance %s swap destruction\n",  filter->name, pidinst->pid->name));
	gf_mx_p(filter->tasks_mx);
	gf_list_del_item(filter->input_pids, pidinst);
	filter->num_input_pids = gf_list_count(filter->input_pids);
	gf_mx_v(filter->tasks_mx);

	gf_mx_p(pid->filter->tasks_mx);
	gf_list_del_item(pid->destinations, pidinst);
	pid->num_destinations = gf_list_count(pid->destinations);
	gf_mx_v(pid->filter->tasks_mx);


	gf_filter_pid_inst_del(pidinst);
	if (pidinst->is_decoder_input) {
		assert(pidinst->pid->nb_decoder_inputs);
		safe_int_dec(&pidinst->pid->nb_decoder_inputs);
	}

	if (filter->num_input_pids) return;

	//filter no longer used, disconnect chain
	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		for (j=0; j<pid->num_destinations; j++) {
			GF_FilterPidInst *a_pidi = gf_list_get(pid->destinations, j);
			if (a_pidi == dst_swapinst) continue;

			gf_filter_pid_inst_swap_delete(a_pidi->filter, pid, a_pidi, dst_swapinst);
		}
	}
	assert(!filter->finalized);
	filter->finalized = GF_TRUE;
	gf_fs_post_task(filter->session, gf_filter_remove_task, filter, NULL, "filter_destroy", NULL);
}

void gf_filter_pid_inst_swap_delete_task(GF_FSTask *task)
{
	GF_FilterPidInst *pidinst = task->udta;
	GF_Filter *filter = pidinst->filter;
	GF_FilterPid *pid = pidinst->pid;
	GF_FilterPidInst *dst_swapinst = pidinst->filter->swap_pidinst_dst;

	//reset in process
	if ((pidinst->filter && pidinst->discard_packets) || filter->stream_reset_pending) {
		TASK_REQUEUE(task)
		return;
	}
	pidinst->filter->swap_pidinst_dst = NULL;

	gf_filter_pid_inst_swap_delete(filter, pid, pidinst, dst_swapinst);
}

void gf_filter_pid_inst_swap(GF_Filter *filter, GF_FilterPidInst *dst)
{
	GF_PropertyMap *prev_dst_props;
	GF_FilterPacketInstance *pcki;
	u32 nb_pck_transfer=0;
	GF_FilterPidInst *src = filter->swap_pidinst_src;
	if (!src) src = filter->swap_pidinst_dst;

	GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s swaping PID %s to PID %s\n", filter->name, src->pid->name, dst->pid->name));
	if (filter->swap_needs_init) {
		//we are in detach state, the pack queue of the old PID is never read
		assert(filter->swap_pidinst_dst->detach_pending);
		//we are in pending stete, the origin of the old PID is never dispatching
		assert(dst->pid->filter->out_pid_connection_pending);
		//we can therefore swap the packet queues safely and other important info
	}
	//otherwise we actually swap the pid instance on the same PID
	else {
		gf_mx_p(dst->pid->filter->tasks_mx);
		gf_list_del_item(dst->pid->destinations, src);
		if (gf_list_find(dst->pid->destinations, dst)<0)
			gf_list_add(dst->pid->destinations, dst);
		if (gf_list_find(dst->filter->input_pids, dst)<0) {
			gf_list_add(dst->filter->input_pids, dst);
			dst->filter->num_input_pids = gf_list_count(dst->filter->input_pids);
		}
		gf_mx_v(dst->pid->filter->tasks_mx);
	}
	assert(!dst->buffer_duration);

	while (1) {
		pcki = gf_fq_pop(src->packets);
		if (!pcki) break;
		assert(src->filter->pending_packets);
		safe_int_dec(&src->filter->pending_packets);

		pcki->pid = dst;
		gf_fq_add(dst->packets, pcki);
		safe_int_inc(&dst->filter->pending_packets);
		nb_pck_transfer++;
	}
	if (src->requires_full_data_block && gf_list_count(src->pck_reassembly)) {
		dst->requires_full_data_block = src->requires_full_data_block;
		dst->last_block_ended = src->last_block_ended;
		dst->first_block_started = src->first_block_started;
		if (!dst->pck_reassembly) dst->pck_reassembly = gf_list_new();
		while (gf_list_count(src->pck_reassembly)) {
			pcki = gf_list_pop_front(src->pck_reassembly);
			pcki->pid = dst;
			gf_list_add(dst->pck_reassembly, pcki);
		}
	}
	dst->is_end_of_stream = src->is_end_of_stream;
	dst->nb_eos_signaled = src->nb_eos_signaled;
	dst->buffer_duration = src->buffer_duration;

	//switch previous src property map to this new pid (this avoids rewriting props of already dispatched packets)
	//it may happen that we already have props on dest, due to configure of the pid
	//use the old props as new ones and merge the previous props of dst in the new props
	prev_dst_props = dst->props;
	dst->props = src->props;
	src->props = NULL;
	if (prev_dst_props) {
		gf_props_merge_property(dst->props, prev_dst_props, NULL, NULL);
		if (safe_int_dec(&prev_dst_props->reference_count)==0) {
			gf_props_del(prev_dst_props);
		}
	}

	if (nb_pck_transfer && !dst->filter->process_task_queued) {
		gf_filter_post_process_task(dst->filter);	
	}

	src = filter->swap_pidinst_dst;
	if (filter->swap_needs_init) {
		//exit out special handling of the pid since we are ready to detach
		assert(src->filter->stream_reset_pending);
		safe_int_dec(&src->filter->stream_reset_pending);

		//post detach task, we will reset the swap_pidinst only once truly deconnected from filter
		gf_fs_post_task(filter->session, gf_filter_pid_detach_task, src->filter, src->pid, "pidinst_detach", filter);
	} else {
		GF_Filter *src_filter = src->filter;
		assert(!src->filter->sticky);
		assert(src->filter->num_input_pids==1);

		gf_filter_pid_inst_del(src);

		filter->swap_pidinst_dst = NULL;
		filter->swap_pidinst_src = NULL;
		assert(!src_filter->finalized);
		src_filter->finalized = GF_TRUE;
		gf_fs_post_task(src_filter->session, gf_filter_remove_task, src_filter, NULL, "filter_destroy", NULL);
	}
	if (filter->swap_pidinst_src) {
		src = filter->swap_pidinst_src;
		src->filter->swap_pidinst_dst = filter->swap_pidinst_dst;
		gf_fs_post_task(filter->session, gf_filter_pid_inst_swap_delete_task, src->filter, src->pid, "pid_inst_delete", src);
	}
}


typedef enum {
	GF_PID_CONF_CONNECT,
	GF_PID_CONF_RECONFIG,
	GF_PID_CONF_REMOVE,
} GF_PidConnectType;

static GF_Err gf_filter_pid_configure(GF_Filter *filter, GF_FilterPid *pid, GF_PidConnectType ctype)
{
	u32 i, count;
	GF_Err e;
	Bool new_pid_inst=GF_FALSE;
	GF_FilterPidInst *pidinst=NULL;

	assert(filter->freg->configure_pid);

	if (filter->detached_pid_inst) {
		count = gf_list_count(filter->detached_pid_inst);
		for (i=0; i<count; i++) {
			pidinst = gf_list_get(filter->detached_pid_inst, i);
			if (pidinst->filter==filter) {
				gf_list_rem(filter->detached_pid_inst, i);
				//reattach new filter and pid
				pidinst->filter = filter;
				pidinst->pid = pid;

				assert(!pidinst->props);

				//and treat as new pid inst
				if (ctype == GF_PID_CONF_CONNECT) {
					new_pid_inst=GF_TRUE;
				}
				safe_int_dec(&pidinst->detach_pending);
				break;
			}
			pidinst=NULL;
		}
		if (! gf_list_count(filter->detached_pid_inst)) {
			gf_list_del(filter->detached_pid_inst);
			filter->detached_pid_inst = NULL;
		}
	}
	if (!pidinst) {
		count = pid->num_destinations;
		for (i=0; i<count; i++) {
			pidinst = gf_list_get(pid->destinations, i);
			if (pidinst->filter==filter) {
				break;
			}
			pidinst=NULL;
		}
	}

	//first connection of this PID to this filter
	if (!pidinst) {
		if (ctype != GF_PID_CONF_CONNECT) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Trying to disconnect PID %s not found in filter %s inputs\n",  pid->name, filter->name));
			return GF_SERVICE_ERROR;
		}
		pidinst = gf_filter_pid_inst_new(filter, pid);
		new_pid_inst=GF_TRUE;
	}

	//if new, add the PID to input/output before calling configure
	if (new_pid_inst) {
		assert(pidinst);
		gf_mx_p(pid->filter->tasks_mx);

		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Registering %s::%s as destination for %s::%s\n", pid->filter->name, pid->name, pidinst->filter->name, pidinst->pid->name));
		gf_list_add(pid->destinations, pidinst);
		pid->num_destinations = gf_list_count(pid->destinations);

		if (!filter->input_pids) filter->input_pids = gf_list_new();
		gf_list_add(filter->input_pids, pidinst);
		filter->num_input_pids = gf_list_count(filter->input_pids);

		gf_mx_v(pid->filter->tasks_mx);

		//new connection, update caps in case we have events using caps (buffer req) being sent
		//while processing the configure (they would be dispatched on the source filter, not the dest one being
		//processed here)
		gf_filter_pid_update_caps(pid);
	}

	//we are swaping a PID instance (dyn insert of a filter), do it before reconnecting
	//in order to have properties in place
	//TODO: handle error case, we might need to re-switch the pid inst!
	if (filter->swap_pidinst_src || filter->swap_pidinst_dst) {
		gf_filter_pid_inst_swap(filter, pidinst);
	}

	//commented out for now, due to audio thread pulling packets out of the pid but not in the compositor:process, which
	//could be called for video at the same time... FIXME
#ifdef FILTER_FIXME
	FSESS_CHECK_THREAD(filter)
#endif
	e = filter->freg->configure_pid(filter, (GF_FilterPid*) pidinst, (ctype==GF_PID_CONF_REMOVE) ? GF_TRUE : GF_FALSE);

#ifdef GPAC_MEMORY_TRACKING
	if (filter->session->check_allocs) {
		if (filter->nb_consecutive_process >= filter->max_nb_consecutive_process) {
			filter->max_nb_consecutive_process = filter->nb_consecutive_process;
			filter->max_nb_process = filter->nb_process_since_reset;
			filter->max_stats_nb_alloc = filter->stats_nb_alloc;
			filter->max_stats_nb_calloc = filter->stats_nb_calloc;
			filter->max_stats_nb_realloc = filter->stats_nb_realloc;
			filter->max_stats_nb_free = filter->stats_nb_free;
		}
		filter->stats_mem_allocated = 0;
		filter->stats_nb_alloc = filter->stats_nb_realloc = filter->stats_nb_free = 0;
		filter->nb_process_since_reset = filter->nb_consecutive_process = 0;
	}
#endif

	if (e==GF_OK) {
		//if new, register the new pid instance, and the source pid as input to this filer
		if (new_pid_inst) {
			GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Connected filter %s (%p) PID %s (%p) (%d fan-out) to filter %s (%p)\n", pid->filter->name, pid->filter, pid->name, pid, pid->num_destinations, filter->name, filter));
		}
	}
	//failure on reconfigure, try reloading a filter chain
	else if (ctype==GF_PID_CONF_RECONFIG) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to reconfigure PID %s:%s in filter %s: %s, reloading filter graph\n", pid->filter->name, pid->name, filter->name, gf_error_to_string(e) ));
		gf_filter_relink_dst(pidinst);
	} else {

		//error,  remove from input and poutput
		gf_mx_p(filter->tasks_mx);
		gf_list_del_item(filter->input_pids, pidinst);
		filter->num_input_pids = gf_list_count(filter->input_pids);
		gf_mx_v(filter->tasks_mx);

		gf_mx_p(pidinst->pid->filter->tasks_mx);
		gf_list_del_item(pidinst->pid->destinations, pidinst);
		pidinst->pid->num_destinations = gf_list_count(pidinst->pid->destinations);
		pidinst->filter = NULL;
		gf_mx_v(pidinst->pid->filter->tasks_mx);

		//if connect and error, direct delete of pid
		if (new_pid_inst) {
			gf_mx_p(pid->filter->tasks_mx);
			gf_list_del_item(pid->destinations, pidinst);
			pid->num_destinations = gf_list_count(pid->destinations);
			gf_filter_pid_inst_del(pidinst);
			gf_mx_v(pid->filter->tasks_mx);
		}


		if (e==GF_REQUIRES_NEW_INSTANCE) {
			//TODO: copy over args from current filter
			GF_Filter *new_filter = gf_filter_clone(filter);
			if (new_filter) {
				GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Clone filter %s, new instance for pid %s\n", filter->name, pid->name));
				gf_filter_pid_post_connect_task(new_filter, pid);
				return GF_OK;
			} else {
				GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to clone filter %s\n", filter->name));
				e = GF_OUT_OF_MEM;
			}
		}
		if (e && (ctype==GF_PID_CONF_REMOVE)) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to disconnect filter %s PID %s from filter %s: %s\n", pid->filter->name, pid->name, filter->name, gf_error_to_string(e) ));
		}
		else if (e) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to connect filter %s PID %s to filter %s: %s\n", pid->filter->name, pid->name, filter->name, gf_error_to_string(e) ));

			if (filter->has_out_caps) {
				Bool unload_filter = GF_TRUE;
				//try to load another filter to handle that connection
				//1-blacklist this filter
				gf_list_add(pid->filter->blacklisted, (void *) filter->freg);
				//2-disconnect all other inputs, and post a re-init
				gf_mx_p(filter->tasks_mx);
				while (gf_list_count(filter->input_pids)) {
					GF_FilterPidInst *a_pidinst = gf_list_pop_back(filter->input_pids);
					FSESS_CHECK_THREAD(filter)
					filter->freg->configure_pid(filter, (GF_FilterPid *) a_pidinst, GF_TRUE);

					gf_filter_pid_post_init_task(a_pidinst->pid->filter, a_pidinst->pid);

					gf_fs_post_task(filter->session, gf_filter_pid_inst_delete_task, a_pidinst->pid->filter, a_pidinst->pid, "pid_inst_delete", a_pidinst);

					unload_filter = GF_FALSE;
				}
				filter->num_input_pids = 0;
				gf_mx_v(filter->tasks_mx);

				if (!filter->session->last_connect_error) filter->session->last_connect_error = e;
				if (ctype==GF_PID_CONF_CONNECT) {
					assert(pid->filter->out_pid_connection_pending);
					safe_int_dec(&pid->filter->out_pid_connection_pending);
				}
				//3- post a re-init on this pid
				gf_filter_pid_post_init_task(pid->filter, pid);

				if (unload_filter) {
					assert(!filter->finalized);
					filter->finalized = GF_TRUE;
					assert(!gf_list_count(filter->input_pids));
					gf_fs_post_task(filter->session, gf_filter_remove_task, filter, NULL, "filter_destroy", NULL);
				}
				return e;
			} else {
				GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to reconfigure input of sink %s, cannot rebuild graph\n", filter->name));
			}
		} else {
			filter->session->last_connect_error = GF_OK;
		}

		//try to run filter no matter what
		if (filter->session->requires_solved_graph )
			return e;
	}

	//flush all pending pid init requests following the call to init
	if (filter->has_pending_pids) {
		filter->has_pending_pids = GF_FALSE;
		while (gf_fq_count(filter->pending_pids)) {
			GF_FilterPid *pid=gf_fq_pop(filter->pending_pids);

			gf_filter_pid_post_init_task(filter, pid);
		}
	}

	if (ctype==GF_PID_CONF_REMOVE) {
		gf_mx_p(filter->tasks_mx);
		gf_list_del_item(filter->input_pids, pidinst);
		filter->num_input_pids = gf_list_count(filter->input_pids);
		gf_mx_v(filter->tasks_mx);

		gf_mx_p(pidinst->pid->filter->tasks_mx);
		gf_list_del_item(pidinst->pid->destinations, pidinst);
		pidinst->pid->num_destinations = gf_list_count(pidinst->pid->destinations);
		pidinst->filter = NULL;
		gf_mx_v(pidinst->pid->filter->tasks_mx);

		//disconnected the last input, flag as removed
		if (!filter->num_input_pids && !filter->sticky)
			filter->removed = GF_TRUE;
		//post a pid_delete task to also trigger removal of the filter if needed
		gf_fs_post_task(filter->session, gf_filter_pid_inst_delete_task, pid->filter, pid, "pid_inst_delete", pidinst);

		return e;
	}

	if (ctype==GF_PID_CONF_CONNECT) {
		assert(pid->filter->out_pid_connection_pending);
		if ( (safe_int_dec(&pid->filter->out_pid_connection_pending)==0) ) {

			//postponed packets dispatched by source while setting up PID, flush through process()
			//pending packets (not yet consumed but in PID buffer), start processing
			if (pid->filter->postponed_packets || pid->filter->pending_packets || pid->filter->nb_caps_renegociate) {
				gf_filter_post_process_task(pid->filter);
			}
		}
	}
	//once all pid have been (re)connected, update any internal caps
	gf_filter_pid_update_caps(pid);
	return e;
}

static void gf_filter_pid_connect_task(GF_FSTask *task)
{
	GF_Filter *filter = task->filter;
	GF_FilterSession *fsess = filter->session;
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s pid %s connecting to %s\n", task->pid->pid->filter->name, task->pid->pid->name, task->filter->name));

	//filter will require a new instance, clone it
	if (filter->num_input_pids && (filter->max_extra_pids <= filter->num_input_pids - 1)) {
		GF_Filter *new_filter = gf_filter_clone(filter);
		if (new_filter) {
			filter = new_filter;
		} else {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to clone filter %s\n", filter->name));
			safe_int_dec(&filter->in_pid_connection_pending);
			return;
		}
	}
	gf_filter_pid_configure(filter, task->pid->pid, GF_PID_CONF_CONNECT);
	//once connected, any set_property before the first packet dispatch will have to trigger a reconfigure
	task->pid->pid->request_property_map = GF_TRUE;
	task->pid->pid->pid_info_changed = GF_FALSE;

	//filter may now be the clone, decrement on original filter
	safe_int_dec(&task->filter->in_pid_connection_pending);

	gf_fs_cleanup_filters(fsess);

}

void gf_filter_pid_reconfigure_task(GF_FSTask *task)
{
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s pid %s reconfigure to %s\n", task->pid->pid->filter->name, task->pid->pid->name, task->filter->name));

	gf_filter_pid_configure(task->filter, task->pid->pid, GF_PID_CONF_RECONFIG);
}

void gf_filter_pid_disconnect_task(GF_FSTask *task)
{
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s pid %s disconnect from %s\n", task->pid->pid->filter->name, task->pid->pid->name, task->filter->name));
	gf_filter_pid_configure(task->filter, task->pid->pid, GF_PID_CONF_REMOVE);

	//if the filter has no more connected ins and outs, remove it
	if (task->filter->removed && !gf_list_count(task->filter->output_pids) && !gf_list_count(task->filter->input_pids)) {
		Bool direct_mode = task->filter->session->direct_mode;
		assert(!task->filter->finalized);
		task->filter->finalized = GF_TRUE;
		gf_fs_post_task(task->filter->session, gf_filter_remove_task, task->filter, NULL, "filter_destroy", NULL);
		if (direct_mode) task->filter = NULL;
	}
}

void gf_filter_pid_detach_task(GF_FSTask *task)
{
	u32 i, count;
	GF_Filter *filter = task->filter;
	GF_FilterPid *pid = task->pid->pid;
	GF_FilterPidInst *pidinst=NULL;
	GF_Filter *new_chain_input = task->udta;

	//we may have concurrent reset (due to play/stop/seek) and caps renegotiation
	//wait for the pid to be reset before detaching
	if (pid->filter->stream_reset_pending) {
		TASK_REQUEUE(task)
		return;
	}
	if (new_chain_input->in_pid_connection_pending) {
		TASK_REQUEUE(task)
		return;
	}

	assert(filter->freg->configure_pid);
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s pid %s detach from %s\n", task->pid->pid->filter->name, task->pid->pid->name, task->filter->name));

	count = pid->num_destinations;
	for (i=0; i<count; i++) {
		pidinst = gf_list_get(pid->destinations, i);
		if (pidinst->filter==filter) {
			break;
		}
		pidinst=NULL;
	}

	//first connection of this PID to this filter
	if (!pidinst) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Trying to detach PID %s not found in filter %s inputs\n",  pid->name, filter->name));
		if (new_chain_input) {
			assert(!new_chain_input->swap_pidinst_dst);
			assert(!new_chain_input->swap_pidinst_src);
			new_chain_input->swap_needs_init = GF_FALSE;
		}
		return;
	}

	//detach props
	if (pidinst->props && (safe_int_dec(& pidinst->props->reference_count) == 0) ) {
		gf_list_del_item(pidinst->pid->properties, pidinst->props);
		gf_props_del(pidinst->props);
	}
	pidinst->props = NULL;

	gf_mx_p(filter->tasks_mx);
	//detach pid
	gf_filter_pid_inst_reset(pidinst);
	pidinst->pid = NULL;
	gf_list_del_item(pid->destinations, pidinst);
	pid->num_destinations = gf_list_count(pid->destinations);
	gf_list_del_item(filter->input_pids, pidinst);
	filter->num_input_pids = gf_list_count(filter->input_pids);
	gf_mx_v(filter->tasks_mx);

	if (!filter->detached_pid_inst) {
		filter->detached_pid_inst = gf_list_new();
	}
	gf_list_add(filter->detached_pid_inst, pidinst);

	//we are done, reset filter swap instance so that connection can take place
	if (new_chain_input->swap_needs_init) {
		new_chain_input->swap_pidinst_dst = NULL;
		new_chain_input->swap_pidinst_src = NULL;
		new_chain_input->swap_needs_init = GF_FALSE;
	}
}

GF_EXPORT
void gf_filter_pid_set_name(GF_FilterPid *pid, const char *name)
{
	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Attempt to assign name %s to input PID %s in filter %s - ignoring\n", name, pid->pid->name, pid->pid->filter->name));
	} else if (name) {
		if (pid->name && name && !strcmp(pid->name, name)) return;
		if (pid->name) gf_free(pid->name);
		pid->name = gf_strdup(name);
	}
}

GF_EXPORT
const char *gf_filter_pid_get_name(GF_FilterPid *pid)
{
	return pid->pid->name;
}

GF_EXPORT
const char *gf_filter_pid_get_filter_name(GF_FilterPid *pid)
{
	if (PID_IS_INPUT(pid)) {
		return pid->pid->filter->name;
	}
	return pid->filter->name;
}

GF_EXPORT
const char *gf_filter_pid_orig_src_args(GF_FilterPid *pid)
{
	u32 i;
	const char *args = pid->pid->filter->src_args;
	if (args && strstr(args, "src")) return args;
	if (!pid->filter->num_input_pids) return args;
	for (i=0; i<pid->pid->filter->num_input_pids; i++) {
		GF_FilterPidInst *pidi = gf_list_get(pid->pid->filter->input_pids, i);
		const char *arg_src = gf_filter_pid_orig_src_args(pidi->pid);
		if (arg_src) return arg_src;
	}
	return args;
}

GF_EXPORT
const char *gf_filter_pid_get_source_filter_name(GF_FilterPid *pid)
{
	GF_Filter *filter  = pid->pid->filter;
	while (filter && filter->num_input_pids) {
		GF_FilterPidInst *pidi = gf_list_get(pid->filter->input_pids, 0);
		filter = pidi->pid->filter;
	}
	if (!filter) return NULL;
	return filter->name ? filter->name : filter->freg->name;
}

GF_EXPORT
void gf_filter_pid_get_buffer_occupancy(GF_FilterPid *pid, u32 *max_slots, u32 *nb_pck, u32 *max_duration, u32 *duration)
{
	if (max_slots) *max_slots = pid->pid->max_buffer_unit;
	if (nb_pck) *nb_pck = pid->pid->nb_buffer_unit;
	if (max_duration) *max_duration = (u32) pid->pid->max_buffer_time;
	if (duration) *duration = (u32) pid->pid->buffer_duration;
}

GF_EXPORT
void gf_filter_pid_set_udta(GF_FilterPid *pid, void *udta)
{
	if (PID_IS_INPUT(pid)) {
		((GF_FilterPidInst *)pid)->udta = udta;
	} else {
		pid->udta = udta;
	}
}

GF_EXPORT
void *gf_filter_pid_get_udta(GF_FilterPid *pid)
{
	if (PID_IS_INPUT(pid)) {
		return ((GF_FilterPidInst *)pid)->udta;
	} else {
		return pid->udta;
	}
}

static Bool filter_pid_check_fragment(GF_FilterPid *src_pid, char *frag_name, Bool *pid_excluded)
{
	char *psep;
	u32 comp_type=0;
	Bool is_neg = GF_FALSE;
	const GF_PropertyValue *prop;

	if (frag_name[0] == src_pid->filter->session->sep_neg) {
		frag_name++;
		is_neg = GF_TRUE;
	}
	//special case for stream types filters
	prop = gf_filter_pid_get_property(src_pid, GF_PROP_PID_STREAM_TYPE);
	if (prop) {
		u32 matched=0;
		u32 type=0;
		if (!strnicmp(frag_name, "audio", 5)) {
			matched=5;
			type=GF_STREAM_AUDIO;
		} else if (!strnicmp(frag_name, "video", 5)) {
			matched=5;
			type=GF_STREAM_VISUAL;
		} else if (!strnicmp(frag_name, "scene", 5)) {
			matched=5;
			type=GF_STREAM_SCENE;
		} else if (!strnicmp(frag_name, "font", 4)) {
			matched=4;
			type=GF_STREAM_FONT;
		} else if (!strnicmp(frag_name, "text", 4)) {
			matched=4;
			type=GF_STREAM_TEXT;
		}
		if (matched && (type != prop->value.uint)) {
			//special case: if we request a non-file stream but the pid is a file, we will need a demux to
			//move from file to A/V/... streams, so we accept any #MEDIA from file streams
			if (prop->value.uint == GF_STREAM_FILE) {
				return GF_TRUE;
			}
			matched = 0;
		}

		if (matched) {
			u32 idx=0;
			u32 k, count_pid;
			if (strlen(frag_name)==matched) return GF_TRUE;
			idx = atoi(frag_name+matched);
			count_pid = src_pid->filter->num_output_pids;
			for (k=0; k<count_pid; k++) {
				GF_FilterPid *p = gf_list_get(src_pid->filter->output_pids, k);
				prop = gf_filter_pid_get_property(src_pid, GF_PROP_PID_STREAM_TYPE);
				if (prop && prop->value.uint==type) {
					idx--;
					if (!idx) {
						if (p==src_pid) return GF_TRUE;
						break;
					}
				}
			}
			*pid_excluded = GF_TRUE;
			return GF_FALSE;
		}
	}
	//special case for codec type filters
	if (!strcmp(frag_name, "raw")) {
		prop = gf_filter_pid_get_property(src_pid, GF_PROP_PID_CODECID);
		if (prop) {
			Bool is_eq = (prop->value.uint==GF_CODECID_RAW) ? GF_TRUE : GF_FALSE;
			if (is_neg) is_eq = !is_eq;
			if (is_eq) return GF_TRUE;
			*pid_excluded = GF_TRUE;
			return GF_FALSE;
		}
		//not codec ID set for pid, assume match
		return GF_TRUE;
	}

	//generic property addressing code(or builtin name)=val
	psep = strchr(frag_name, src_pid->filter->session->sep_name);
	if (!psep) {
		psep = strchr(frag_name, '-');
		if (psep) comp_type = 1;
		else {
			psep = strchr(frag_name, '+');
			if (psep) comp_type = 2;
		}
	}

	if (!psep) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("PID addressing %s not recognized, ignoring and assuming match\n", frag_name ));
		return GF_TRUE;
	}

	Bool is_equal = GF_FALSE;
	Bool use_not_equal = GF_FALSE;
	GF_PropertyValue prop_val;
	u32 p4cc = 0;
	char c=psep[0];
	psep[0] = 0;
	prop=NULL;

	//check for built-in property
	p4cc = gf_props_get_id(frag_name);
	if (!p4cc && !strcmp(frag_name, "PID") )
		p4cc = GF_PROP_PID_ID;

	if (!p4cc && (strlen(frag_name)==4))
		p4cc = GF_4CC(frag_name[0], frag_name[1], frag_name[2], frag_name[3]);

	if (p4cc) prop = gf_filter_pid_get_property(src_pid, p4cc);
	//not a built-in property, find prop by name
	if (!prop) {
		prop = gf_filter_pid_get_property_str(src_pid, frag_name);
	}

	psep[0] = c;

	//if the property is not found, we accept the connection
	if (!prop) {
		return GF_TRUE;
	}

	//check for negation
	if ( (psep[0]==src_pid->filter->session->sep_name) && (psep[1]==src_pid->filter->session->sep_neg) ) {
		psep++;
		use_not_equal = GF_TRUE;
	}
	//parse the property, based on its property type
	prop_val = gf_props_parse_value(prop->type, frag_name, psep+1, NULL, src_pid->filter->session->sep_list);
	if (!comp_type) {
		is_equal = gf_props_equal(prop, &prop_val);
		if (use_not_equal) is_equal = !is_equal;
	} else {
		switch (prop_val.type) {
		case GF_PROP_SINT:
			if (prop->value.sint<prop_val.value.sint) is_equal = GF_TRUE;
			if (comp_type==2) is_equal = !is_equal;
			break;
		case GF_PROP_UINT:
			if (prop->value.uint<prop_val.value.uint) is_equal = GF_TRUE;
			if (comp_type==2) is_equal = !is_equal;
			break;
		case GF_PROP_LSINT:
			if (prop->value.longsint<prop_val.value.longsint) is_equal = GF_TRUE;
			if (comp_type==2) is_equal = !is_equal;
			break;
		case GF_PROP_LUINT:
			if (prop->value.longuint<prop_val.value.longuint) is_equal = GF_TRUE;
			if (comp_type==2) is_equal = !is_equal;
			break;
		case GF_PROP_FLOAT:
			if (prop->value.fnumber<prop_val.value.fnumber) is_equal = GF_TRUE;
			if (comp_type==2) is_equal = !is_equal;
			break;
		case GF_PROP_DOUBLE:
			if (prop->value.number<prop_val.value.number) is_equal = GF_TRUE;
			if (comp_type==2) is_equal = !is_equal;
			break;
		case GF_PROP_FRACTION:
			if (prop->value.frac.num * prop_val.value.frac.den < prop->value.frac.den * prop_val.value.frac.num) is_equal = GF_TRUE;
			if (comp_type == 2) is_equal = !is_equal;
			break;
		case GF_PROP_FRACTION64:
			if (prop->value.lfrac.num * prop_val.value.lfrac.den < prop->value.lfrac.den * prop_val.value.lfrac.num) is_equal = GF_TRUE;
			if (comp_type == 2) is_equal = !is_equal;
			break;
		default:
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("PID addressing uses \'%s\' comparison on property %s which is not a number, defaulting to equal=true\n", (comp_type==1) ? "less than" : "more than", gf_props_4cc_get_name(p4cc) ));
			is_equal = GF_TRUE;
			break;
		}
	}
	gf_props_reset_single(&prop_val);
	if (!is_equal) *pid_excluded = GF_TRUE;

	return is_equal;
}

static Bool filter_source_id_match(GF_FilterPid *src_pid, const char *id, const char *source_ids, Bool *pid_excluded)
{
	*pid_excluded = GF_FALSE;
	if (!source_ids)
		return GF_TRUE;
	if (!id)
		return GF_FALSE;
	while (source_ids) {
		Bool all_matched = GF_TRUE;
		u32 len, sublen;
		Bool last=GF_FALSE;
		char *sep = strchr(source_ids, src_pid->filter->session->sep_list);
		char *frag_name;
		if (sep) {
			len = (u32) (sep - source_ids);
		} else {
			len = (u32) strlen(source_ids);
			last=GF_TRUE;
		}

		frag_name = strchr(source_ids, src_pid->filter->session->sep_frag);
		if (frag_name > source_ids + len) frag_name = NULL;
		sublen = frag_name ? (u32) (frag_name - source_ids) : len;
		//skip frag char
		if (frag_name) frag_name++;

		//any ID, always match
		if (source_ids[0]=='*') { }
		// id does not match
		else if (strncmp(id, source_ids, sublen)) {
			source_ids += len;
			if (last) break;
		}
		if (!frag_name) return GF_TRUE;

		//match pid name
		if (!strcmp(src_pid->name, frag_name)) return GF_TRUE;

		//for all listed fragment extensions
		while (frag_name && all_matched) {
			char *next_frag = strchr(frag_name, src_pid->filter->session->sep_frag);
			if (next_frag) next_frag[0] = 0;

			if (! filter_pid_check_fragment(src_pid, frag_name, pid_excluded))
				all_matched = GF_FALSE;

			if (!next_frag) break;

			next_frag[0] = src_pid->filter->session->sep_frag;
			frag_name = next_frag+1;
		}
		return all_matched;
	}
	return GF_FALSE;
}

Bool filter_in_parent_chain(GF_Filter *parent, GF_Filter *filter)
{
	u32 i, count;
	if (parent == filter) return GF_TRUE;
	//browse all parent PIDs
	count = parent->num_input_pids;
	if (!count) return GF_FALSE;
	for (i=0; i<count; i++) {
		GF_FilterPidInst *pid = gf_list_get(parent->input_pids, i);
		if (filter_in_parent_chain(pid->pid->filter, filter)) return GF_TRUE;
	}
	return GF_FALSE;
}

Bool gf_filter_pid_caps_match(GF_FilterPid *src_pid, const GF_FilterRegister *freg, GF_Filter *filter_inst, u8 *priority, u32 *dst_bundle_idx, GF_Filter *dst_filter, s32 for_bundle_idx)
{
	u32 i=0;
	u32 cur_bundle_start = 0;
	u32 cap_bundle_idx = 0;
	u32 nb_subcaps=0;
	Bool skip_explicit_load = GF_FALSE;
	Bool all_caps_matched = GF_TRUE;
	Bool forced_cap_found = src_pid->forced_cap ? GF_FALSE : GF_TRUE;
	const GF_FilterCapability *in_caps;
	u32 nb_in_caps;

	if (!freg) {
		assert(dst_filter);
		freg = dst_filter->freg;
		skip_explicit_load = GF_TRUE;
	}
	in_caps = freg->caps;
	nb_in_caps = freg->nb_caps;
	if (filter_inst && (filter_inst->freg==freg)) {
		skip_explicit_load = GF_TRUE;
		if (filter_inst->forced_caps) {
			in_caps = filter_inst->forced_caps;
			nb_in_caps = filter_inst->nb_forced_caps;
		}
	}

	if (filter_inst && filter_inst->encoder_stream_type) {
		const GF_PropertyValue *pid_st = gf_filter_pid_get_property(src_pid, GF_PROP_PID_STREAM_TYPE);
		if (pid_st && (pid_st->value.uint != filter_inst->encoder_stream_type))
			return GF_FALSE;
	}

	if (priority)
		(*priority) = freg->priority;

	if (dst_bundle_idx)
		(*dst_bundle_idx) = 0;

	//filters with no explicit input cap accept anything for now, this should be refined ...
	if (!in_caps)
		return GF_TRUE;

	//check all input caps of dst filter
	for (i=0; i<nb_in_caps; i++) {
		const GF_PropertyValue *pid_cap=NULL;
		const GF_FilterCapability *cap = &in_caps[i];

		if (i && !(cap->flags & GF_CAPFLAG_IN_BUNDLE) ) {
			if (all_caps_matched && forced_cap_found) {
				if (dst_bundle_idx)
					(*dst_bundle_idx) = cap_bundle_idx;
				return GF_TRUE;
			}
			all_caps_matched = GF_TRUE;
			nb_subcaps=0;
			cur_bundle_start = i;
			cap_bundle_idx++;
			if ((for_bundle_idx>=0) && (cap_bundle_idx > (u32) for_bundle_idx)) {
				break;
			}
			continue;
		}
		if ((for_bundle_idx>=0) && (cap_bundle_idx < (u32) for_bundle_idx)) {
			all_caps_matched = 0;
			continue;
		}

		//not an input cap
		if (! (cap->flags & GF_CAPFLAG_INPUT) ) {
			if (!skip_explicit_load && (cap->flags & GF_CAPFLAG_LOADED_FILTER) ) {
				all_caps_matched = 0;
			}
			continue;
		}

		nb_subcaps++;
		//no match for this cap, go on until new one or end
		if (!all_caps_matched) continue;

		if (cap->code) {
			if (!forced_cap_found && (cap->code==src_pid->forced_cap))
				forced_cap_found = GF_TRUE;

			pid_cap = gf_filter_pid_get_property(src_pid, cap->code);
		}

		//optional cap
		if (cap->flags & GF_CAPFLAG_OPTIONAL) continue;

		//try by name
		if (!pid_cap && cap->name) pid_cap = gf_filter_pid_get_property_str(src_pid, cap->name);

		if (src_pid->ext_not_trusted && (cap->code==GF_PROP_PID_FILE_EXT)) {
			all_caps_matched=GF_FALSE;
			continue;
		}


		//we found a property of that type and it is equal
		if (pid_cap) {
			u32 j;
			Bool prop_excluded = GF_FALSE;
			Bool prop_equal = GF_FALSE;

			//this could be optimized by not checking several times the same cap
			for (j=0; j<nb_in_caps; j++) {
				const GF_FilterCapability *a_cap = &in_caps[j];

				if ((j>cur_bundle_start) && ! (a_cap->flags & GF_CAPFLAG_IN_BUNDLE) ) {
					break;
				}
				//not an input cap
				if (! (a_cap->flags & GF_CAPFLAG_INPUT) ) continue;
				//optional cap
				if (a_cap->flags & GF_CAPFLAG_OPTIONAL) continue;
				//not a static and not in bundle
				if (! (a_cap->flags & GF_CAPFLAG_STATIC)) {
					if (j<cur_bundle_start)
						continue;
				}

				if (cap->code) {
					if (cap->code!=a_cap->code) continue;
				} else if (!cap->name || !a_cap->name || strcmp(cap->name, a_cap->name)) {
					continue;
				}
				if (!skip_explicit_load && (a_cap->flags & GF_CAPFLAG_LOADED_FILTER) ) {
					if (!dst_filter || (dst_filter != src_pid->filter->dst_filter)) {
						prop_equal = GF_FALSE;
						break;
					}
					if (dst_filter->freg != freg) {
						prop_equal = GF_FALSE;
						break;
					}
				}

				if (!prop_equal) {
					prop_equal = gf_props_equal(pid_cap, &a_cap->val);
					//excluded cap: if value match, don't match this cap at all
					if (a_cap->flags & GF_CAPFLAG_EXCLUDED) {
						if (prop_equal) {
							prop_equal = GF_FALSE;
							prop_excluded = GF_FALSE;
							break;
						}
						prop_excluded = GF_TRUE;
					}
					if (prop_equal) break;
				}
			}
			if (!prop_equal && !prop_excluded) {
				all_caps_matched=GF_FALSE;
			} else if (priority && cap->priority) {
				(*priority) = cap->priority;
			}
		}
		else if (!(cap->flags & GF_CAPFLAG_EXCLUDED) ) {
			all_caps_matched=GF_FALSE;
		}
	}

	if (nb_subcaps && all_caps_matched && forced_cap_found) {
		if (dst_bundle_idx)
			(*dst_bundle_idx) = cap_bundle_idx;
		return GF_TRUE;
	}

	return GF_FALSE;
}

GF_Err gf_filter_pid_force_cap(GF_FilterPid *pid, u32 cap4cc)
{
	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Cannot force PID cap on input PID\n"));
		return GF_BAD_PARAM;
	}
	if (pid->num_destinations) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_MEDIA, ("Cannot force PID cap on already connected pid\n"));
		return GF_BAD_PARAM;
	}
	pid->forced_cap = cap4cc;
	return GF_OK;
}

u32 gf_filter_caps_bundle_count(const GF_FilterCapability *caps, u32 nb_caps)
{
	u32 i, nb_bundles = nb_caps ? 1 : 0;
	for (i=0; i<nb_caps; i++) {
		const GF_FilterCapability *cap = &caps[i];
		if (! (cap->flags & GF_CAPFLAG_IN_BUNDLE)) {
			nb_bundles++;
			continue;
		}
	}
	return nb_bundles;
}

Bool gf_filter_has_out_caps(const GF_FilterRegister *freg)
{
	u32 i;
	//check all input caps of dst filter, count bundles
	for (i=0; i<freg->nb_caps; i++) {
		const GF_FilterCapability *out_cap = &freg->caps[i];
		if (out_cap->flags & GF_CAPFLAG_OUTPUT) {
			return GF_TRUE;
		}
	}
	return GF_FALSE;
}


u32 gf_filter_caps_to_caps_match(const GF_FilterRegister *src, u32 src_bundle_idx, const GF_FilterRegister *dst_reg, GF_Filter *dst_filter, u32 *dst_bundle_idx, s32 for_dst_bundle, u32 *loaded_filter_flags, GF_CapsBundleStore *capstore)
{
	u32 i=0;
	u32 cur_bundle_start = 0;
	u32 cur_bundle_idx = 0;
	u32 nb_matched=0;
	u32 nb_out_caps=0;
	u32 nb_in_bundles=0;
	u32 bundle_score = 0;
	u32 *bundles_in_ok = NULL;
	u32 *bundles_cap_found = NULL;
	u32 *bundles_in_scores = NULL;
	//initialize caps matched to true for first cap bundle
	Bool all_caps_matched = GF_TRUE;
	const GF_FilterCapability *dst_caps = dst_reg->caps;
	u32 nb_dst_caps = dst_reg->nb_caps;

	if (dst_filter && dst_filter->freg==dst_reg && dst_filter->forced_caps) {
		dst_caps = dst_filter->forced_caps;
		nb_dst_caps = dst_filter->nb_forced_caps;
	}

	//check all input caps of dst filter, count bundles
	if (! gf_filter_has_out_caps(src)) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s has no output caps, cannot match filter %s inputs\n", src->name, dst_reg->name));
		return 0;
	}

	//check all input caps of dst filter, count bundles
	nb_in_bundles = gf_filter_caps_bundle_count(dst_caps, nb_dst_caps);
	if (!nb_in_bundles) {
		if (dst_reg->configure_pid) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s has no caps but pid configure possible, assuming possible connection\n", dst_reg->name));
			return 1;
		}
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s has no caps and no pid configure, no possible connection\n", dst_reg->name));
		return 0;
	}
	if (capstore->nb_allocs < nb_in_bundles) {
		capstore->nb_allocs = nb_in_bundles;
		capstore->bundles_in_ok = gf_realloc(capstore->bundles_in_ok, sizeof(u32) * nb_in_bundles);
		capstore->bundles_cap_found = gf_realloc(capstore->bundles_cap_found, sizeof(u32) * nb_in_bundles);
		capstore->bundles_in_scores = gf_realloc(capstore->bundles_in_scores,  sizeof(u32) * nb_in_bundles);
	}
	bundles_in_ok =	capstore->bundles_in_ok;
	bundles_cap_found = capstore->bundles_cap_found;
	bundles_in_scores = capstore->bundles_in_scores;

	for (i=0; i<nb_in_bundles; i++) {
		bundles_in_ok[i] = 1;
		bundles_cap_found[i] = 0;
		bundles_in_scores[i] = 0;
	}


	//check all output caps of src filter
	for (i=0; i<src->nb_caps; i++) {
		u32 j, k;
		Bool already_tested = GF_FALSE;
		const GF_FilterCapability *out_cap = &src->caps[i];

		if (!(out_cap->flags & GF_CAPFLAG_IN_BUNDLE) ) {
			all_caps_matched = GF_TRUE;
			cur_bundle_start = i+1;
			cur_bundle_idx++;
			if (src_bundle_idx < cur_bundle_idx)
				break;

			continue;
		}

		//not our selected output and not static cap
		if ((src_bundle_idx != cur_bundle_idx) && ! (out_cap->flags & GF_CAPFLAG_STATIC) ) {
			continue;
		}

		//not an output cap
		if (!(out_cap->flags & GF_CAPFLAG_OUTPUT) ) continue;

		//no match possible for this cap, wait until next cap start
		if (!all_caps_matched) continue;

		//check we didn't test a cap with same name/code before us
		for (k=cur_bundle_start; k<i; k++) {
			const GF_FilterCapability *an_out_cap = &src->caps[k];
			if (! (an_out_cap->flags & GF_CAPFLAG_IN_BUNDLE) ) {
				break;
			}
			if (! (an_out_cap->flags & GF_CAPFLAG_OUTPUT) ) {
				continue;
			}
			if (out_cap->code && (out_cap->code==an_out_cap->code) ) {
				already_tested = GF_TRUE;
				break;
			}
			if (out_cap->name && an_out_cap->name && !strcmp(out_cap->name, an_out_cap->name)) {
				already_tested = GF_TRUE;
				break;
			}
		}
		if (already_tested) {
			continue;
		}
		nb_out_caps++;

		//set cap as OK in all bundles
		for (k=0; k<nb_in_bundles; k++) {
			bundles_cap_found[k] = 0;
		}

		//check all output caps in this bundle with the same code/name, consider OK if one is matched
		for (k=cur_bundle_start; k<src->nb_caps; k++) {
			u32 cur_dst_bundle=0;
			Bool static_matched = GF_FALSE;
			u32 nb_caps_tested = 0;
			u32 cap_loaded_filter_only = 0;
			Bool matched=GF_FALSE;
			Bool exclude=GF_FALSE;
			Bool prop_found=GF_FALSE;
			const GF_FilterCapability *an_out_cap = &src->caps[k];
			if (! (an_out_cap->flags & GF_CAPFLAG_IN_BUNDLE) ) {
				break;
			}
			if (! (an_out_cap->flags & GF_CAPFLAG_OUTPUT) ) {
				continue;
			}
			if (out_cap->code && (out_cap->code!=an_out_cap->code) )
				continue;
			if (out_cap->name && (!an_out_cap->name || strcmp(out_cap->name, an_out_cap->name)))
				continue;

			//not our selected output and not static cap
			if ((src_bundle_idx != cur_bundle_idx) && ! (an_out_cap->flags & GF_CAPFLAG_STATIC) ) {
				continue;
			}

			nb_matched = 0;
			//check all input caps of dst filter, count ones that are matched
			for (j=0; j<nb_dst_caps; j++) {
				Bool prop_equal;
				const GF_FilterCapability *in_cap = &dst_caps[j];

				if (! (in_cap->flags & GF_CAPFLAG_IN_BUNDLE)) {
					//we found a prop, excluded but with != value hence acceptable, default matching to true
					if (!matched && prop_found) matched = GF_TRUE;
					//not match, flag this bundle as not ok
					if (matched) {
						if (!bundles_cap_found[cur_dst_bundle])
							bundles_cap_found[cur_dst_bundle] = cap_loaded_filter_only ? 2 : 1;

						nb_matched++;
					}

					matched = static_matched ? GF_TRUE : GF_FALSE;
					exclude = GF_FALSE;
					prop_found = GF_FALSE;
					nb_caps_tested = 0;
					cur_dst_bundle++;
					if ((for_dst_bundle>=0) && (cur_dst_bundle > (u32) for_dst_bundle))
						break;

					continue;
				}
				//not an input cap
				if (!(in_cap->flags & GF_CAPFLAG_INPUT) )
					continue;

				//optionnal cap, ignore
				if (in_cap->flags & GF_CAPFLAG_OPTIONAL)
					continue;

				if ((for_dst_bundle>=0) && (cur_dst_bundle < (u32)for_dst_bundle) && !(in_cap->flags & GF_CAPFLAG_STATIC))
					continue;

				//prop was excluded, cannot match in bundle
				if (exclude) continue;
				//prop was matched, no need to check other caps in the current bundle
				if (matched) continue;

				if (out_cap->code && (out_cap->code!=in_cap->code) )
					continue;
				if (out_cap->name && (!in_cap->name || strcmp(out_cap->name, in_cap->name)))
					continue;

				nb_caps_tested++;
				//we found a property of that type , check if equal equal
				prop_equal = gf_props_equal(&in_cap->val, &an_out_cap->val);
				if ((in_cap->flags & GF_CAPFLAG_EXCLUDED) && !(an_out_cap->flags & GF_CAPFLAG_EXCLUDED) ) {
					//prop type matched, output includes it and input excludes it: no match, don't look any further
					if (prop_equal) {
						matched = GF_FALSE;
						exclude = GF_TRUE;
						prop_found = GF_FALSE;
					} else {
						//remember we found a prop of same type but excluded value
						// we will match unless we match an excluded value
						prop_found = GF_TRUE;
					}
				} else if (!(in_cap->flags & GF_CAPFLAG_EXCLUDED) && (an_out_cap->flags & GF_CAPFLAG_EXCLUDED) ) {
					//prop type matched, input includes it and output excludes it: no match, don't look any further
					if (prop_equal) {
						matched = GF_FALSE;
						exclude = GF_TRUE;
						prop_found = GF_FALSE;
					} else {
						//remember we found a prop of same type but excluded value
						//we will match unless we match an excluded value
						prop_found = GF_TRUE;
					}
				} else if (prop_equal) {
					matched = GF_TRUE;
//					if (an_out_cap->flags & GF_CAPFLAG_STATIC)
//						static_matched = GF_TRUE;
				} else if ((in_cap->flags & GF_CAPFLAG_EXCLUDED) && (an_out_cap->flags & GF_CAPFLAG_EXCLUDED) ) {
					//prop type matched, input excludes it and output excludes it and no match, remmeber we found the prop type
					prop_found = GF_TRUE;
				}

				if (prop_found && (in_cap->flags & GF_CAPFLAG_LOADED_FILTER))
					cap_loaded_filter_only = 1;
			}
			if (nb_caps_tested) {
				//we found a prop, excluded but with != value hence acceptable, default matching to true
				if (!matched && prop_found) matched = GF_TRUE;
				//not match, flag this bundle as not ok
				if (matched) {
					if (!bundles_cap_found[cur_dst_bundle])
						bundles_cap_found[cur_dst_bundle] = cap_loaded_filter_only ? 2 : 1;

					nb_matched++;
				}
			} else if (!nb_dst_caps) {
				if (!bundles_cap_found[cur_dst_bundle])
					bundles_cap_found[cur_dst_bundle] = cap_loaded_filter_only ? 2 : 1;

				nb_matched++;
			}
		}
		//merge bundle cap
		nb_matched=0;
		for (k=0; k<nb_in_bundles; k++) {
			if (!bundles_cap_found[k])
				bundles_in_ok[k] = 0;
			else {
				nb_matched += 1;
				//we matched this property, keep score for the bundle
				bundles_in_scores[k] ++;
				//mark if connection is only valid for loaded inputs
				if (bundles_cap_found[k]==2)
				 	bundles_in_ok[k] |= 1<<1;
				//mark if connection is only valid for loaded outputs
				if (out_cap->flags & GF_CAPFLAG_LOADED_FILTER)
					bundles_in_ok[k] |= 1<<2;
			}
		}
		//not matched and not excluded, skip until next bundle
		if (!nb_matched && !(out_cap->flags & GF_CAPFLAG_EXCLUDED)) {
			all_caps_matched = GF_FALSE;
		}
	}

	//get bundle with highest score
	bundle_score = 0;
	nb_matched = 0;
	for (i=0; i<nb_in_bundles; i++) {
		if (bundles_in_ok[i]) {
			nb_matched++;
			if (bundle_score < bundles_in_scores[i]) {
				*dst_bundle_idx = i;
				bundle_score = bundles_in_scores[i];
				if (loaded_filter_flags) {
					*loaded_filter_flags = (bundles_in_ok[i]>>1);
				}
			}
		}
	}
	if (!bundle_score) {
//		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s outputs cap bundle %d do not match filter %s inputs\n", src->name, src_bundle_idx, dst_reg->name));
	} else {
//		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s outputs cap bundle %d matches filter %s inputs caps bundle %d (%d total bundle matches, bundle matched %d/%d caps)\n", src->name, src_bundle_idx, dst_reg->name, *dst_bundle_idx, nb_matched, bundle_score, nb_out_caps));
	}
	return bundle_score;
}

GF_EXPORT
Bool gf_filter_pid_check_caps(GF_FilterPid *pid)
{
	u8 priority;
	if (PID_IS_OUTPUT(pid)) return GF_FALSE;
	return gf_filter_pid_caps_match(pid->pid, NULL, pid->filter, &priority, NULL, pid->filter, -1);
}


static void concat_reg(GF_FilterSession *sess, char prefRegistry[1001], const char *reg_key, const char *args)
{
	u32 len;
	char *forced_reg, *sep;
	if (!args) return;
	forced_reg = strstr(args, reg_key);
	if (!forced_reg) return;
	forced_reg += 6;
	sep = strchr(forced_reg, sess->sep_args);
	len = sep ? (u32) (sep-forced_reg) : (u32) strlen(forced_reg);
	if (len+2+strlen(prefRegistry)>1000) {
		return;
	}
	if (prefRegistry[0]) {
		char szSepChar[2];
		szSepChar[0] = sess->sep_args;
		szSepChar[1] = 0;
		strcat(prefRegistry, szSepChar);
	}
	strncat(prefRegistry, forced_reg, len);
}

static Bool gf_filter_out_caps_solved_by_connection(const GF_FilterRegister *freg, u32 bundle_idx)
{
	u32 i, k, cur_bundle_idx = 0;
	for (i=0; i<freg->nb_caps; i++) {
		u32 nb_caps = 0;
		const GF_FilterCapability *cap = &freg->caps[i];
		if (!(cap->flags & GF_CAPFLAG_IN_BUNDLE)) {
			cur_bundle_idx++;
			if (cur_bundle_idx>bundle_idx) return GF_FALSE;
		}
		if (!(cap->flags & GF_CAPFLAG_STATIC) && (bundle_idx>cur_bundle_idx)) continue;
		if (!(cap->flags & GF_CAPFLAG_OUTPUT)) continue;
		for (k=0; k<freg->nb_caps; k++) {
			const GF_FilterCapability *acap = &freg->caps[k];
			if (!(acap->flags & GF_CAPFLAG_IN_BUNDLE)) break;
			if (!(acap->flags & GF_CAPFLAG_OUTPUT)) continue;
			if (!(acap->flags & GF_CAPFLAG_STATIC) && (k<i) ) continue;

			if (cap->code && (acap->code==cap->code)) {
				nb_caps++;
			} else if (cap->name && acap->name && !strcmp(cap->name, acap->name)) {
				nb_caps++;
			}
			//if more than one cap with same code in same bundle, consider the filter is undecided
			if (nb_caps>1)
				return GF_TRUE;
		}
	}
	return GF_FALSE;
}

enum
{
	EDGE_STATUS_NONE=0,
	EDGE_STATUS_ENABLED,
	EDGE_STATUS_DISABLED,
};

#define EDGE_LOADED_SOURCE_ONLY (1)
#define EDGE_LOADED_DEST_ONLY (1<<1)

typedef struct
{
	struct __freg_desc *src_reg;
	u16 src_cap_idx;
	u16 dst_cap_idx;
	u8 weight;
	u8 status;
	u8 priority;
	u8 loaded_filter_only;
	//stream type of the output cap of src. Might be:
	// -1 if multiple stream types are defined in the cap (demuxers, encoders/decoders bundles)
	// 0 if not spcified
	// or a valid GF_STREAM_*
	s32 src_stream_type;
} GF_FilterRegEdge;

typedef struct __freg_desc
{
	const GF_FilterRegister *freg;
	u32 nb_edges, nb_alloc_edges;
	GF_FilterRegEdge *edges;
	u32 dist;
	u8 priority;
	u8 edges_marked_rlevel;
	struct __freg_desc *destination;
	u32 cap_idx;
} GF_FilterRegDesc;

static s32 gf_filter_reg_get_output_stream_type(const GF_FilterRegister *freg, u32 out_cap_idx)
{
	u32 i, cur_bundle, stype=0, nb_stype=0;

	cur_bundle = 0;
	for (i=0; i<freg->nb_caps; i++) {
		u32 cap_stype=0;
		const GF_FilterCapability *cap = &freg->caps[i];
		if (!(cap->flags & GF_CAPFLAG_IN_BUNDLE)) {
			cur_bundle++;
			continue;
		}
		if (!(cap->flags & GF_CAPFLAG_OUTPUT)) continue;

		if ((cur_bundle != out_cap_idx) && !(cap->flags & GF_CAPFLAG_STATIC) ) continue;
		//output type is file or same media type, allow looking for filter chains
		if (cap->flags & GF_CAPFLAG_EXCLUDED) continue;
		if (cap->code == GF_PROP_PID_STREAM_TYPE) cap_stype = cap->val.value.uint;
		else if (cap->code == GF_PROP_PID_MIME) cap_stype = GF_STREAM_FILE;
		else if (cap->code == GF_PROP_PID_FILE_EXT) cap_stype = GF_STREAM_FILE;

		if (!cap_stype) continue;

		if (stype != cap_stype) {
			stype = cap_stype;
			nb_stype++;
		}
	}
	if (nb_stype==1) return (s32) stype;
	if (nb_stype) return -1;
	return 0;
}

static Bool gf_filter_pid_enable_edges(GF_FilterSession *fsess, GF_FilterRegDesc *reg_desc, u32 src_cap_idx, const GF_FilterRegister *src_freg, u32 rlevel, s32 dst_stream_type)
{
	u32 i=0;
	Bool break_loop = (reg_desc->edges_marked_rlevel < rlevel) ? GF_TRUE: GF_FALSE;

	if (src_freg == reg_desc->freg) {
		return GF_TRUE;
	}
	if (rlevel > fsess->max_resolve_chain_len) {
		return GF_FALSE;
	}

	reg_desc->edges_marked_rlevel = rlevel;

	for (i=0; i<reg_desc->nb_edges; i++) {
		GF_FilterRegEdge *edge = &reg_desc->edges[i];
		if (!break_loop && (edge->dst_cap_idx == src_cap_idx) && (edge->status == EDGE_STATUS_NONE)) {
			//candidate edge, check stream type
			s32 source_stream_type = edge->src_stream_type;

			//source edge cap indicates multiple stream types (demuxer/encoder/decoder dundle)
			if (source_stream_type<0) {
				//if destination type is known (>=0 and NOT file, inherit it
				//otherwise, we we can't filter out yet
				if ((dst_stream_type>0) && (dst_stream_type != GF_STREAM_FILE))
					source_stream_type = dst_stream_type;
			}
			//inherit source type if not specified
			if (!source_stream_type && dst_stream_type>0)
				source_stream_type = dst_stream_type;
			//if source is encrypted type and dest type is set, use dest type
			if ((source_stream_type==GF_STREAM_ENCRYPTED) && dst_stream_type>0)
				source_stream_type = dst_stream_type;
			//if dest is encrypted type and source type is set, use source type
			if ((dst_stream_type==GF_STREAM_ENCRYPTED) && source_stream_type>0)
				dst_stream_type = source_stream_type;

			//if stream types are know (>0) and not source files, do not mark the edges if they mismatch
			//moving from non-file type A to non-file type B requires an explicit filter
			if ((dst_stream_type>0) && (source_stream_type>0) && (source_stream_type != GF_STREAM_FILE) && (dst_stream_type != GF_STREAM_FILE) && (source_stream_type != dst_stream_type)) {
				continue;
			}
			if (gf_filter_pid_enable_edges(fsess, edge->src_reg, edge->src_cap_idx, src_freg, rlevel+1, source_stream_type)) {
				edge->status = EDGE_STATUS_ENABLED;
			}
		}
	}
	return GF_TRUE;
}


static GF_FilterRegDesc *gf_filter_reg_build_graph(GF_List *links, const GF_FilterRegister *freg, GF_CapsBundleStore *capstore, GF_FilterPid *src_pid, GF_Filter *dst_filter)
{
	u32 nb_dst_caps, nb_regs, i;
	Bool freg_has_output = gf_filter_has_out_caps(freg);
	GF_FilterRegDesc *reg_desc = NULL;

	GF_SAFEALLOC(reg_desc, GF_FilterRegDesc);
	reg_desc->freg = freg;

	nb_dst_caps = gf_filter_caps_bundle_count(freg->caps, freg->nb_caps);


	//we are building a registry descriptor acting as destination, ignore any output caps
	if (src_pid || dst_filter) freg_has_output = GF_FALSE;

	//setup all connections
	nb_regs = gf_list_count(links);
	for (i=0; i<nb_regs; i++) {
		u32 nb_src_caps, k, l;
		u32 path_weight;

		GF_FilterRegDesc *a_reg = gf_list_get(links, i);

		//check which cap of this filter matches our destination
		nb_src_caps = gf_filter_caps_bundle_count(a_reg->freg->caps, a_reg->freg->nb_caps);
		for (k=0; k<nb_src_caps; k++) {
			for (l=0; l<nb_dst_caps; l++) {
				s32 bundle_idx;

				if ( gf_filter_has_out_caps(a_reg->freg)) {
					u32 loaded_filter_only_flags = 0;

					path_weight = gf_filter_caps_to_caps_match(a_reg->freg, k, (const GF_FilterRegister *) freg, dst_filter, &bundle_idx, l, &loaded_filter_only_flags, capstore);

					if (path_weight && (bundle_idx == l)) {
						GF_FilterRegEdge *edge;
						if (reg_desc->nb_edges==reg_desc->nb_alloc_edges) {
							reg_desc->nb_alloc_edges += 10;
							reg_desc->edges = gf_realloc(reg_desc->edges, sizeof(GF_FilterRegEdge) * reg_desc->nb_alloc_edges);
						}
						assert(path_weight<0xFF);
						assert(k<0xFFFF);
						assert(l<0xFFFF);
						edge = &reg_desc->edges[reg_desc->nb_edges];
						edge->src_reg = a_reg;
						edge->weight = path_weight;
						edge->src_cap_idx = k;
						edge->dst_cap_idx = l;
						edge->priority = 0;
						edge->loaded_filter_only = loaded_filter_only_flags;
					 	edge->src_stream_type = gf_filter_reg_get_output_stream_type(edge->src_reg->freg, edge->src_cap_idx);

						reg_desc->nb_edges++;
					}
				}

				if ( freg_has_output ) {
					u32 loaded_filter_only_flags = 0;
					path_weight = gf_filter_caps_to_caps_match(freg, l, a_reg->freg, dst_filter, &bundle_idx, k, &loaded_filter_only_flags, capstore);

					if (path_weight && (bundle_idx == k)) {
						GF_FilterRegEdge *edge;
						if (a_reg->nb_edges==a_reg->nb_alloc_edges) {
							a_reg->nb_alloc_edges += 10;
							a_reg->edges = gf_realloc(a_reg->edges, sizeof(GF_FilterRegEdge) * a_reg->nb_alloc_edges);
						}
						edge = &a_reg->edges[a_reg->nb_edges];
						edge->src_reg = reg_desc;
						edge->weight = (u8) path_weight;
						edge->src_cap_idx = (u16) l;
						edge->dst_cap_idx = (u16) k;
						edge->priority = 0;
						edge->loaded_filter_only = loaded_filter_only_flags;
					 	edge->src_stream_type = gf_filter_reg_get_output_stream_type(edge->src_reg->freg, edge->src_cap_idx);

						a_reg->nb_edges++;
					}
				}
			}
		}
	}
	return reg_desc;
}

void gf_filter_sess_build_graph(GF_FilterSession *fsess, const GF_FilterRegister *for_reg)
{
	u32 i, count;
	GF_CapsBundleStore capstore;
	memset(&capstore, 0, sizeof(GF_CapsBundleStore));

	if (!fsess->links) fsess->links = gf_list_new();

	if (for_reg) {
		GF_FilterRegDesc *freg_desc = gf_filter_reg_build_graph(fsess->links, for_reg, &capstore, NULL, NULL);
		if (!freg_desc) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to build graph entry for filter %s\n", for_reg->name));
		} else {
			gf_list_add(fsess->links, freg_desc);
		}
	} else {
		u64 start_time = gf_sys_clock_high_res();
		count = gf_list_count(fsess->registry);
		for (i=0; i<count; i++) {
			const GF_FilterRegister *freg = gf_list_get(fsess->registry, i);
			GF_FilterRegDesc *freg_desc = gf_filter_reg_build_graph(fsess->links, freg, &capstore, NULL, NULL);
			if (!freg_desc) {
				GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Failed to build graph entry for filter %s\n", freg->name));
			} else {
				gf_list_add(fsess->links, freg_desc);
			}
		}
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Build filter graph in "LLU" us\n", gf_sys_clock_high_res() - start_time));
	}
	if (capstore.bundles_cap_found) gf_free(capstore.bundles_cap_found);
	if (capstore.bundles_in_ok) gf_free(capstore.bundles_in_ok);
	if (capstore.bundles_in_scores) gf_free(capstore.bundles_in_scores);
}

void gf_filter_sess_reset_graph(GF_FilterSession *fsess, const GF_FilterRegister *freg)
{
	gf_mx_p(fsess->links_mx);
	if (freg) {
		s32 reg_idx=-1;
		u32 i, count = gf_list_count(fsess->links);
		for (i=0; i<count; i++) {
			u32 j;
			GF_FilterRegDesc *rdesc = gf_list_get(fsess->links, i);
			if (rdesc->freg == freg) {
				reg_idx = i;
				continue;
			}
			for (j=0; j<rdesc->nb_edges; j++) {
				if (rdesc->edges[j].src_reg->freg == freg) {
					if (rdesc->nb_edges > j + 1) {
						memmove(&rdesc->edges[j], &rdesc->edges[j+1], sizeof (GF_FilterRegEdge) * (rdesc->nb_edges - j - 1));
					}
					j--;
					rdesc->nb_edges--;
				}
			}
		}
		if (reg_idx>=0) {
			GF_FilterRegDesc *rdesc = gf_list_get(fsess->links, reg_idx);
			gf_list_rem(fsess->links, reg_idx);
			gf_free(rdesc->edges);
			gf_free(rdesc);
		}
	} else {
		while (gf_list_count(fsess->links)) {
			GF_FilterRegDesc *rdesc = gf_list_pop_back(fsess->links);
			gf_free(rdesc->edges);
			gf_free(rdesc);
		}
	}
	gf_mx_v(fsess->links_mx);
}


static void gf_filter_pid_resolve_link_dijkstra(GF_FilterPid *pid, GF_Filter *dst, const char *prefRegistry, Bool reconfigurable_only, GF_List *out_reg_chain)
{
	GF_FilterRegDesc *reg_dst, *result;
	GF_List *dijkstra_nodes;
	GF_FilterSession *fsess = pid->filter->session;
	//build all edges
	u32 i, dijsktra_node_count, dijsktra_edge_count, count = gf_list_count(fsess->registry);
	GF_CapsBundleStore capstore;
	Bool first;
	u32 path_weight, max_weight=0;
	u64 dijkstra_time_us, sort_time_us, start_time_us = gf_sys_clock_high_res();

	if (!fsess->links || ! gf_list_count( fsess->links))
	 	gf_filter_sess_build_graph(fsess, NULL);

	dijkstra_nodes = gf_list_new();

	result = NULL;

	//1: select all elligible filters for the graph resolution: exclude sources, sinks, explicits, blacklisted and not reconfigurable if we reconfigure
	count = gf_list_count( fsess->links);
	for (i=0; i<count; i++) {
		u32 j;
		GF_FilterRegDesc *reg_desc = gf_list_get(fsess->links, i);
		const GF_FilterRegister *freg = reg_desc->freg;

		//reset node state
		reg_desc->destination = NULL;
		reg_desc->cap_idx = 0;
		reg_desc->edges_marked_rlevel = fsess->max_resolve_chain_len;
		reg_desc->dist = -1;
		reg_desc->priority = 0xFF;

		//remember our source descriptor - it may be absent of the final node set in case we want reconfigurable only filters
		//and the source is not reconfigurable
		if (freg == pid->filter->freg)
			result = reg_desc;

		//don't add source filters except if PID is from source
		if (!freg->configure_pid && (freg!=pid->filter->freg)) {
			assert(freg != dst->freg);
			continue;
		}

		//freg shall be instantiated
		if ((freg->flags & GF_FS_REG_EXPLICIT_ONLY) && (freg != pid->filter->freg) && (freg != dst->freg) ) {
			assert(freg != dst->freg);
			continue;
		}

		//no output caps, cannot add
		if ((freg != dst->freg) && !gf_filter_has_out_caps(freg)) {
			assert(freg != dst->freg);
			assert(freg != pid->filter->freg);
			continue;
		}

		//we only want reconfigurable output filters
		if (reconfigurable_only && !freg->reconfigure_output && (freg != dst->freg)) {
			assert(freg != dst->freg);
			continue;
		}

		//blacklisted filter, can't add pid
		if (gf_list_find(pid->filter->blacklisted, (void *) freg)>=0) {
			assert(freg != dst->freg);
			assert(freg != pid->filter->freg);
			continue;
		}

		if (pid->adapters_blacklist && (gf_list_find(pid->adapters_blacklist, (void *) freg)>=0)) {
			assert(freg != dst->freg);
			continue;
		}

		//reset edge status - if source is our origin, disable edge if cap is not matched
		for (j=0; j<reg_desc->nb_edges; j++) {
			GF_FilterRegEdge *edge = &reg_desc->edges[j];
			edge->status = EDGE_STATUS_NONE;
			//connection from source, disable edge if pid caps mismatch
			if (edge->src_reg->freg == pid->filter->freg) {
				u8 priority=0;
				u32 dst_bundle_idx;
				//check path weight for the given dst cap - we MUST give the target cap otherwise we might get a default match to another cap
				path_weight = gf_filter_pid_caps_match(pid, freg, NULL, &priority, &dst_bundle_idx, pid->filter->dst_filter, edge->dst_cap_idx);
				if (!path_weight) {
					edge->status = EDGE_STATUS_DISABLED;
					continue;
				}
			}

			if (edge->loaded_filter_only && (edge->src_reg->freg != pid->filter->freg) ) {
				edge->status = EDGE_STATUS_DISABLED;
				continue;
			}

			if ((u32) edge->weight + 1 > max_weight)
				max_weight = (u32) edge->weight + 1;
		}

		//do not add destination filter
		if (dst->freg == reg_desc->freg) {
			reg_desc->dist = 0;
			reg_desc->priority = 0;
		} else {
			gf_list_add(dijkstra_nodes, reg_desc);
		}
	}
	//create a new node for the destination based on elligible filters in the graph
	memset(&capstore, 0, sizeof(GF_CapsBundleStore));
	reg_dst = gf_filter_reg_build_graph(dijkstra_nodes, dst->freg, &capstore, pid, dst);
	reg_dst->dist = 0;
	reg_dst->priority = 0;
	reg_dst->edges_marked_rlevel = fsess->max_resolve_chain_len;

	//enable edges of destination, potentially disabling edges from source filters to dest
	for (i=0; i<reg_dst->nb_edges; i++) {
		GF_FilterRegEdge *edge = &reg_dst->edges[i];
		edge->status = EDGE_STATUS_NONE;

		if (edge->loaded_filter_only) {
			edge->status = EDGE_STATUS_DISABLED;
			continue;
		}
		//connection from source, disable edge if pid caps mismatch
		if (edge->src_reg->freg == pid->filter->freg) {
			u8 priority=0;
			u32 dst_bundle_idx;
			path_weight = gf_filter_pid_caps_match(pid, dst->freg, dst, &priority, &dst_bundle_idx, pid->filter->dst_filter, -1);
			if (!path_weight)
				continue;

			if (dst_bundle_idx != edge->dst_cap_idx) {
				edge->status = EDGE_STATUS_DISABLED;
				continue;
			}
		}
		if ((u32) edge->weight + 1 > max_weight)
			max_weight = edge->weight + 1;
		//enable edge and propagate down the graph
		edge->status = EDGE_STATUS_ENABLED;

		gf_filter_pid_enable_edges(fsess, edge->src_reg, edge->src_cap_idx, pid->filter->freg, 1, edge->src_stream_type);
	}

	if (capstore.bundles_cap_found) gf_free(capstore.bundles_cap_found);
	if (capstore.bundles_in_ok) gf_free(capstore.bundles_in_ok);
	if (capstore.bundles_in_scores) gf_free(capstore.bundles_in_scores);

	if (fsess->flags & GF_FS_FLAG_PRINT_CONNECTIONS) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s sources: ", reg_dst->freg->name));
		for (i=0; i<reg_dst->nb_edges; i++) {
			GF_FilterRegEdge *edge = &reg_dst->edges[i];
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, (" %s(%d,%d,%d->%d)", edge->src_reg->freg->name, edge->status, edge->weight, edge->src_cap_idx, edge->dst_cap_idx));
		}
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("\n"));

		count = gf_list_count(dijkstra_nodes);
		for (i=0; i<count; i++) {
			u32 j;
			GF_FilterRegDesc *rdesc = gf_list_get(dijkstra_nodes, i);
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s sources: ", rdesc->freg->name));
			for (j=0; j<rdesc->nb_edges; j++) {
				GF_FilterRegEdge *edge = &rdesc->edges[j];
				GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, (" %s(%d,%d,%d->%d)", edge->src_reg->freg->name, edge->status, edge->weight, edge->src_cap_idx, edge->dst_cap_idx));
			}
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("\n"));
		}
	}

	//remove all filters not used for this resolution (no enabled edges)
	count = gf_list_count(dijkstra_nodes);
	for (i=0; i<count; i++) {
		u32 j, nb_edges;
		GF_FilterRegDesc *rdesc = gf_list_get(dijkstra_nodes, i);

		nb_edges = 0;
		for (j=0; j<rdesc->nb_edges; j++) {
			GF_FilterRegEdge *edge = &rdesc->edges[j];
			if (edge->status == EDGE_STATUS_ENABLED) {
				nb_edges++;
				break;
			}
		}

		//remove from registries if no edge enabled, unless our source
		if (!nb_edges && (rdesc->freg != pid->filter->freg)) {
			gf_list_rem(dijkstra_nodes, i);
			i--;
			count--;
		}
	}

	sort_time_us = gf_sys_clock_high_res();


	dijsktra_edge_count = 0;
	dijsktra_node_count = gf_list_count(dijkstra_nodes)+1;
	first = GF_TRUE;
	//OK we have the weighted graph, perform a dijkstra on the graph - we assign by weight, and if same weight we check the priority
	while (1) {
		GF_FilterRegDesc *current_node = NULL;
		u32 reg_idx = -1;
		u32 min_dist = -1;

		count = gf_list_count(dijkstra_nodes);
		if (!count) break;

		if (first) {
			current_node = reg_dst;
		} else {
			//pick up shortest distance
			for (i=0; i<count; i++) {
				GF_FilterRegDesc *reg_desc = gf_list_get(dijkstra_nodes, i);
				if (reg_desc->dist < min_dist) {
					min_dist = reg_desc->dist;
					current_node = reg_desc;
					reg_idx = i;
				}
			}
			//remove current
			if (!current_node)
				break;
			gf_list_rem(dijkstra_nodes, reg_idx);
		}

		if (current_node->freg == pid->filter->freg) {
			result = current_node;
		}
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("[Filters] Dijkstra: testing filter %s\n", current_node->freg->name));

		//compute distances
		for (i=0; i<current_node->nb_edges; i++) {
			u8 priority=0;
			GF_FilterRegEdge *redge = &current_node->edges[i];
			u32 dist;
			Bool do_switch = GF_FALSE;
			dijsktra_edge_count++;

			if (redge->status != EDGE_STATUS_ENABLED)
				continue;

			dist = current_node->dist + 1;//(max_weight - redge->weight);
			if (current_node->freg->flags & GF_FS_REG_HIDE_WEIGHT) {
				dist = current_node->dist;
			}

			priority = redge->priority;
			if (redge->src_reg->freg == pid->filter->freg) {
				s32 dst_bundle_idx;
				if (gf_filter_pid_caps_match(pid, current_node->freg, NULL, &priority, &dst_bundle_idx, dst, redge->dst_cap_idx)) {

				} else {
					continue;
				}
			}

			if (dist < redge->src_reg->dist) do_switch = GF_TRUE;
			else if (dist == redge->src_reg->dist) {
				if (prefRegistry[0] && (redge->src_reg->destination != current_node) && strstr(prefRegistry, current_node->freg->name)) {
					do_switch = GF_TRUE;
					priority = 0;
				} else if ( (dist == redge->src_reg->dist) && (redge->priority < redge->src_reg->priority) )
					do_switch = GF_TRUE;
			}

			if (do_switch) {
				GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("[Filters] Dijkstra: assign filter %s distance %d destination to %s priority %d (previous destination %s distance %d priority %d)\n", redge->src_reg->freg->name, dist, current_node->freg->name, redge->priority, redge->src_reg->destination ? redge->src_reg->destination->freg->name : "none", redge->src_reg->dist, redge->src_reg->priority ));
				redge->src_reg->dist = dist;
				redge->src_reg->priority = priority;
				redge->src_reg->destination = current_node;
				redge->src_reg->cap_idx = redge->src_cap_idx;
			} else if (fsess->flags & GF_FS_FLAG_PRINT_CONNECTIONS) {
				GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("[Filters] Dijkstra: no shorter path from filter %s distance %d from destination %s priority %d (tested %s dist %d priority %d)\n", redge->src_reg->freg->name, redge->src_reg->dist, redge->src_reg->destination ? redge->src_reg->destination->freg->name : "none", redge->priority, current_node->freg->name, dist, redge->src_reg->priority));

			}
		}
		first = GF_FALSE;
	}

	sort_time_us -= start_time_us;
	dijkstra_time_us = gf_sys_clock_high_res() - start_time_us;
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("[Filters] Dijkstra: sorted filters in "LLU" us, Dijkstra done in "LLU" us on %d nodes %d edges\n", sort_time_us, dijkstra_time_us, dijsktra_node_count, dijsktra_edge_count));

	if (result && result->destination) {
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("[Filters] Dijkstra result: %s", result->freg->name));
		result = result->destination;
		while (result->destination) {
			GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, (" %s", result->freg->name ));
			gf_list_add(out_reg_chain, (void *) result->freg);
			gf_list_add(out_reg_chain, (void *) &result->freg->caps[result->cap_idx]);
			result = result->destination;
		}
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, (" %s\n", result->freg->name));
	} else {
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("[Filters] Dijkstra: no results found!\n"));
	}
	gf_list_del(dijkstra_nodes);

	gf_free(reg_dst->edges);
	gf_free(reg_dst);
}


/*
	!Resolves a link between a PID and a destination filter

\param pid source pid to connect
\param dst destination filter to connect source PID to
\param filter_reassigned indicates the filter has been destroyed and reassigned
\param reconfigurable_only indicates the chain should be loaded for reconfigurable filters
\return the first filter in the matching chain, or NULL if no match
*/
static GF_Filter *gf_filter_pid_resolve_link_internal(GF_FilterPid *pid, GF_Filter *dst, Bool *filter_reassigned, Bool reconfigurable_only, u32 *min_chain_len)
{
	GF_Filter *chain_input = NULL;
	GF_FilterSession *fsess = pid->filter->session;
	GF_List *filter_chain;
	u32 i, count;
	char prefRegistry[1001];
	char szForceReg[20];

	if (!fsess->max_resolve_chain_len) return NULL;

	filter_chain = gf_list_new();

	if (filter_reassigned)
		*filter_reassigned = GF_FALSE;

	if (!dst) return NULL;

	sprintf(szForceReg, "gfreg%c", pid->filter->session->sep_name);
	prefRegistry[0]=0;
	//look for reg given in
	concat_reg(pid->filter->session, prefRegistry, szForceReg, pid->filter->orig_args ? pid->filter->orig_args : pid->filter->src_args);
	concat_reg(pid->filter->session, prefRegistry, szForceReg, pid->filter->dst_args);
	concat_reg(pid->filter->session, prefRegistry, szForceReg, dst->src_args);
	concat_reg(pid->filter->session, prefRegistry, szForceReg, dst->dst_args);

	gf_mx_p(fsess->links_mx);
	gf_filter_pid_resolve_link_dijkstra(pid, dst, prefRegistry, reconfigurable_only, filter_chain);
	gf_mx_v(fsess->links_mx);

	count = gf_list_count(filter_chain);
	if (min_chain_len) {
		*min_chain_len = count;
	} else if (count==0) {
		Bool can_reassign = GF_TRUE;

		//reassign only for source filters
		if (pid->filter->num_input_pids) can_reassign = GF_FALSE;
		//sticky filters cannot be unloaded
		else if (pid->filter->sticky) can_reassign = GF_FALSE;
		//if we don't have pending PIDs to setup from the source
		else if (pid->filter->out_pid_connection_pending) can_reassign = GF_FALSE;
		//if we don't have pending PIDs to setup from the source
		else if (pid->filter->num_output_pids) {
			u32 k;
			for (k=0; k<pid->filter->num_output_pids; k++) {
				GF_FilterPid *apid = gf_list_get(pid->filter->output_pids, k);
				if (apid->num_destinations) can_reassign = GF_FALSE;
				else if ((apid==pid) && (apid->init_task_pending>1)) can_reassign = GF_FALSE;
				else if ((apid!=pid) && apid->init_task_pending) can_reassign = GF_FALSE;
				if (!can_reassign)
					break;
			}
		}
		//if source filter, try to load another filter - we should complete this with a cache of filter sources
		if (filter_reassigned && can_reassign) {
			if (! gf_filter_swap_source_registry(pid->filter) ) {
				//no filter found for this pid !
				GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("No suitable filter chain found\n"));
			} else {
				GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Swap source demux to %s\n", pid->filter->freg->name));
			}
			*filter_reassigned = GF_TRUE;
		} else if (!reconfigurable_only) {
			GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("No suitable filter found for pid %s from filter %s\n", pid->name, pid->filter->name));
		}
	} else if (reconfigurable_only && (count>2)) {
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Cannot find filter chain with only one filter handling reconfigurable output for pid %s from filter %s - not supported\n", pid->name, pid->filter->name));
	} else {
		const char *dst_args = NULL;
		const char *args = pid->filter->orig_args ? pid->filter->orig_args : pid->filter->src_args;
		GF_FilterPid *a_pid = pid;
		GF_Filter *prev_af;

		dst_args = dst->src_args ? dst->src_args : dst->orig_args;

		while (a_pid) {
			GF_FilterPidInst *pidi;
			args = a_pid->filter->src_args;
			if (!args) args = a_pid->filter->orig_args;
			if (args) break;
			pidi = gf_list_get(a_pid->filter->input_pids, 0);
			if (!pidi) break;
			a_pid = pidi->pid;
		}

#ifndef GPAC_DISABLE_LOGS
		if (gf_log_tool_level_on(GF_LOG_FILTER, GF_LOG_INFO)) {
			GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Solved %sfilter chain from filter %s PID %s to filter %s - dumping chain:\n", reconfigurable_only ? "adaptation " : "", pid->filter->name, pid->name, dst->freg->name));
		}
#endif
		prev_af = NULL;
		for (i=0; i<count; i++) {
			GF_Filter *af;
			Bool load_first_only = GF_FALSE;
			s32 cap_idx = -1;
			const GF_FilterRegister *freg;
			const GF_FilterCapability *cap = NULL;
			u32 k, cur_bundle, bundle_idx=0;
			if (i%2) continue;
			freg = gf_list_get(filter_chain, i);
			cap = gf_list_get(filter_chain, i + 1);
			//get the cap bundle index - the cap added to the list is the cap with the same index as the bundle start we want
			//(this avoids allocating integers to store the bundle)
			for (k=0; k<freg->nb_caps; k++) {
				if (&freg->caps[k]==cap) {
					bundle_idx = k;
					break;
				}
			}
			cur_bundle = 0;
			for (k=0; k<freg->nb_caps; k++) {
				cap = &freg->caps[k];
				if (cur_bundle==bundle_idx) {
					cap_idx = k;
					break;
				}
				if (!(cap->flags & GF_CAPFLAG_IN_BUNDLE)) {
					cur_bundle++;
				}
			}
			//if first filter has multiple possible outputs, don't bother loading the entire chain since it is likely wrong
			//(eg demuxers, we don't know yet what's in the file)
			if (!i && gf_filter_out_caps_solved_by_connection(freg, bundle_idx))
				load_first_only = GF_TRUE;

			GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("\t%s\n", freg->name));

			af = gf_filter_new(fsess, freg, args, dst_args, GF_FILTER_ARG_INHERIT, NULL);
			if (!af) goto exit;

			//the other filters shouldn't need any specific init
			af->dynamic_filter = GF_TRUE;
//			af->user_pid_props = GF_FALSE;
			//remember our target cap on that filter
			af->cap_idx_at_resolution = cap_idx;
			//copy source IDs for all filters in the chain
			//we cannot figure out the destination sourceID when initializing PID connection tasks
			//by walking up the filter chain because PIDs connection might be pending
			//(upper chain not yet fully connected)
			if (dst->source_ids)
				af->source_ids = gf_strdup(dst->source_ids);

			//remember our target filter
			if (prev_af) prev_af->dst_filter = af;
			if (i+2==count) af->dst_filter = dst;

			//also remember our original target in case we got the link wrong
			af->target_filter = pid->filter->target_filter;

			prev_af = af;

			if (reconfigurable_only) af->is_pid_adaptation_filter = GF_TRUE;

			//remember the first in the chain
			if (!i) chain_input = af;

			if (load_first_only) {
				GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s needs to be connected to decide its outputs, not loading end of the chain\n", freg->name));
				break;
			}
		}
	}

exit:
	gf_list_del(filter_chain);
	return chain_input;
}

GF_Filter *gf_filter_pid_resolve_link(GF_FilterPid *pid, GF_Filter *dst, Bool *filter_reassigned)
{
	return gf_filter_pid_resolve_link_internal(pid, dst, filter_reassigned, GF_FALSE, NULL);
}

GF_Filter *gf_filter_pid_resolve_link_for_caps(GF_FilterPid *pid, GF_Filter *dst)
{
	return gf_filter_pid_resolve_link_internal(pid, dst, NULL, GF_TRUE, NULL);
}

u32 gf_filter_pid_resolve_link_length(GF_FilterPid *pid, GF_Filter *dst)
{
	u32 chain_len=0;
	gf_filter_pid_resolve_link_internal(pid, dst, NULL, GF_FALSE, &chain_len);
	return chain_len;
}

static void gf_filter_pid_set_args(GF_Filter *filter, GF_FilterPid *pid)
{
	char *args;
	if (!filter->src_args && !filter->orig_args) return;
	args = filter->orig_args ? filter->orig_args : filter->src_args;
	//parse each arg
	while (args) {
		u32 p4cc=0;
		u32 prop_type=GF_PROP_FORBIDEN;
		char *eq;
		char *value, *name;
		//look for our arg separator
		char *sep = strchr(args, filter->session->sep_args);

		if (filter->session->sep_args == ':') {
			while (sep && !strncmp(sep, "://", 3)) {
				//get root /
				sep = strchr(sep+3, '/');
				//get first : after root
				if (sep) sep = strchr(sep+1, ':');
			}

			//watchout for "C:\\"
			while (sep && (sep[1]=='\\')) {
				sep = strchr(sep+1, ':');
			}
		}

		if (sep) {
			char *xml_start = strchr(args, '<');
			u32 len = (u32) (sep-args);
			if (xml_start) {
				u32 xlen = (u32) (xml_start-args);
				if ((xlen < len) && (args[len-1] != '>')) {
					while (1) {
						sep = strchr(sep+1, filter->session->sep_args);
						if (!sep) {
							break;
						}
						len = (u32) (sep-args);
						if (args[len-1] == '>')
							break;
					}
				}

			}
		}

		if (sep) sep[0]=0;

		if (args[0] != filter->session->sep_frag)
			goto skip_arg;

		eq = strchr(args, filter->session->sep_name);
		if (!eq)
			goto skip_arg;

		eq[0]=0;
		value = eq+1;
		name = args+1;

		if (strlen(name)==4) {
			p4cc = GF_4CC(name[0], name[1], name[2], name[3]);
			if (p4cc) prop_type = gf_props_4cc_get_type(p4cc);
		}
		if (prop_type==GF_PROP_FORBIDEN) {
			p4cc = gf_props_get_id(name);
			if (p4cc) prop_type = gf_props_4cc_get_type(p4cc);
		}

		if (prop_type != GF_PROP_FORBIDEN) {
			GF_PropertyValue p = gf_props_parse_value(prop_type, name, value, NULL, pid->filter->session->sep_list);
			if (prop_type==GF_PROP_NAME) {
				p.type = GF_PROP_STRING;
				gf_filter_pid_set_property(pid, p4cc, &p);
				p.type = GF_PROP_NAME;
			} else {
				gf_filter_pid_set_property(pid, p4cc, &p);
			}
			if (prop_type==GF_PROP_STRING_LIST) {
				p.value.string_list = NULL;
			}
			else if (prop_type==GF_PROP_UINT_LIST) {
				p.value.uint_list.vals = NULL;
			}
			gf_props_reset_single(&p);
		} else {
			GF_PropertyValue p;
			memset(&p, 0, sizeof(GF_PropertyValue));
			p.type = GF_PROP_STRING;
			p.value.string = eq+1;
			gf_filter_pid_set_property_dyn(pid, name, &p);
		}
		eq[0] = filter->session->sep_name;

skip_arg:
		if (sep) {
			sep[0]=0;
			args=sep+1;
		} else {
			break;
		}
	}
}
static const char *gf_filter_last_id_in_chain(GF_Filter *filter)
{
	u32 i;
	const char *id;
	if (filter->id) return filter->id;
	if (!filter->dynamic_filter) return NULL;

	for (i=0; i<filter->num_input_pids; i++) {
		GF_FilterPidInst *pidi = gf_list_get(filter->input_pids, i);
		if (pidi->pid->filter->id) return pidi->pid->filter->id;
		//stop at first non dyn filter
		if (!pidi->pid->filter->dynamic_filter) continue;
		id = gf_filter_last_id_in_chain(pidi->pid->filter);
		if (id) return id;
	}
	return NULL;
}

void gf_filter_pid_retry_caps_negotiate(GF_FilterPid *src_pid, GF_FilterPid *pid, GF_Filter *dst_filter)
{
	assert(dst_filter);
	src_pid->caps_negociate = pid->caps_negociate;
	pid->caps_negociate = NULL;
	src_pid->caps_dst_filter = dst_filter;
	//blacklist filter for adaptation
	if (!src_pid->adapters_blacklist) src_pid->adapters_blacklist = gf_list_new();
	gf_list_add(src_pid->adapters_blacklist, (void *) pid->filter->freg);
	//once != 0 will trigger reconfiguration, so set this once all vars have been set
	safe_int_inc(& src_pid->filter->nb_caps_renegociate );

	//disconnect source pid from filter - this will unload the filter itself
	gf_fs_post_task(src_pid->filter->session, gf_filter_pid_disconnect_task, pid->filter, src_pid, "pidinst_disconnect", NULL);
}


static Bool gf_filter_pid_needs_explicit_resolution(GF_FilterPid *pid, GF_Filter *dst)
{
	u32 i;
	const GF_FilterCapability *caps;
	u32 nb_caps;
	const GF_PropertyValue *p = gf_filter_pid_get_property(pid, GF_PROP_PID_STREAM_TYPE);
	if (!p) return GF_TRUE;

	if (p->value.uint==GF_STREAM_FILE) return GF_FALSE;

	caps = dst->forced_caps ? dst->forced_caps : dst->freg->caps;
	nb_caps = dst->forced_caps ? dst->nb_forced_caps : dst->freg->nb_caps;
	for (i=0; i<nb_caps; i++) {
		const GF_FilterCapability *cap = &caps[i];
		if (!(cap->flags & GF_CAPFLAG_INPUT)) continue;

		if (cap->code != GF_PROP_PID_STREAM_TYPE) continue;
		//output type is file or same media type, allow looking for filter chains
		if ((cap->val.value.uint==GF_STREAM_FILE) || (cap->val.value.uint==p->value.uint)) return GF_FALSE;
	}
	//no mathing type found, we will need an explicit filter to solve this link (ie the link will be to the explicit filter)
	return GF_TRUE;
}

static void gf_filter_pid_init_task(GF_FSTask *task)
{
	u32 i, count;
	Bool found_dest=GF_FALSE;
	Bool can_try_link_resuolution=GF_FALSE;
	Bool first_pass=GF_TRUE;
	GF_List *loaded_filters = NULL;
	GF_Filter *filter = task->filter;
	GF_FilterPid *pid = task->pid;
	Bool filter_found_but_pid_excluded = GF_FALSE;
	const char *filter_id;

	if (pid->destroyed) {
		safe_int_dec(&pid->init_task_pending);
		return;
	}
	pid->props_changed_since_connect = GF_FALSE;

	//swap pid is pending on the possible destination filter
	if (filter->swap_pidinst_src || filter->swap_pidinst_dst) {
		task->requeue_request = GF_TRUE;
		return;
	}
	if (filter->caps_negociate) {
		if (! gf_filter_reconf_output(filter, pid))
			return;
	}

	if (filter->user_pid_props)
		gf_filter_pid_set_args(filter, pid);


	//since we may have inserted filters in the middle (demuxers typically), get the last explicitely
	//loaded ID in the chain
	filter_id = gf_filter_last_id_in_chain(filter);
	if (!filter_id && filter->cloned_from)
		filter_id = gf_filter_last_id_in_chain(filter->cloned_from);

restart:

	if (filter->session->filters_mx) gf_mx_p(filter->session->filters_mx);

	if (!first_pass)
		loaded_filters = gf_list_new();

	//try to connect pid to all running filters
	count = gf_list_count(filter->session->filters);
	for (i=0; i<count; i++) {
		Bool cap_matched = GF_FALSE;
		GF_Filter *filter_dst = gf_list_get(filter->session->filters, i);
		//source filter
		if (!filter_dst->freg->configure_pid) continue;
		if (filter_dst->finalized || filter_dst->removed) continue;

		//if destination accepts only one input, and connected or connection pending
		if (!filter_dst->max_extra_pids && (filter_dst->num_input_pids || filter_dst->in_pid_connection_pending) ) {
			//not explicitly clonable, don't connect to it
			if (!filter_dst->clonable)
				continue;

			//explicitly clonable but caps don't match, don't connect to it
			if (!gf_filter_pid_caps_match(pid, filter_dst->freg, NULL, NULL, NULL, pid->filter->dst_filter, -1))
				continue;
		}

		if (gf_list_find(pid->filter->blacklisted, (void *) filter_dst->freg)>=0) continue;

		//we don't allow re-entrant filter registries (eg filter foo of type A output cannot connect to filter bar of type A)
		if (pid->pid->filter->freg == filter_dst->freg) {
			continue;
		}
		//we try to load a filter chain, so don't test against filters loaded for another chain
		if (filter_dst->dynamic_filter && (filter_dst != pid->filter->dst_filter)) {
			//dst was explicitely set and does not match
			if (pid->filter->dst_filter) continue;
			//dst was not set, we may try to connect to this filter if it allows several input
			//this is typically the case for muxers instantiated dynamically
			if (!filter_dst->max_extra_pids) continue;
		}
		//pid->filter->dst_filter NULL and pid->filter->target_filter is not: we had a wrong resolved chain to target
		//so only attempt to relink the chain if dst_filter is the expected target
		if (!pid->filter->dst_filter && pid->filter->target_filter && (filter_dst != pid->filter->target_filter)) {
			if (filter_dst->target_filter != pid->filter->target_filter) {
				continue;
			}
			//if the target filter of this filter is the same as ours, try to connect - typically scalable streams decoding
		}

		//dynamic filters only connect to their destination, unless explicit connections through sources
		//we could remove this but this highly complicates PID resolution
		if (!filter_dst->source_ids && pid->filter->dynamic_filter && pid->filter->dst_filter && (filter_dst!=pid->filter->dst_filter)) continue;

		//walk up through the parent graph and check if this filter is already in. If so don't connect
		//since we don't allow re-entrant PIDs
		if (filter_in_parent_chain(filter, filter_dst) ) continue;

		//if the original filter is in the parent chain of this PID's filter, don't connect (equivalent to re-entrant)
		if (filter_dst->cloned_from) {
			if (filter_in_parent_chain(filter, filter_dst->cloned_from) ) continue;
		}

		//if the filter is in the parent chain of this PID's original filter, don't connect (equivalent to re-entrant)
		if (filter->cloned_from) {
			if (filter_in_parent_chain(filter->cloned_from, filter_dst) ) continue;
		}

		//if we have sourceID info on the destination, check them
		if (filter_id) {
			if (filter_dst->source_ids) {
				Bool pid_excluded=GF_FALSE;
				if (!filter_source_id_match(pid, filter_id, filter_dst->source_ids, &pid_excluded)) {
					if (pid_excluded && first_pass) filter_found_but_pid_excluded = GF_TRUE;
					continue;
				}
			}
			//if no source ID on the dst filter, this means the dst filter accepts any possible connections from out filter
		}
		//no filterID and dst expects only specific filters, continue
		else if (filter_dst->source_ids) {
			Bool pid_excluded=GF_FALSE;
			if (filter_dst->source_ids[0]!='*')
				continue;
			if (!filter_source_id_match(pid, "*", filter_dst->source_ids, &pid_excluded)) {
				if (pid_excluded && first_pass) filter_found_but_pid_excluded = GF_TRUE;
				continue;
			}
		}

		can_try_link_resuolution = GF_TRUE;

		//we have a match, check if caps are OK
		cap_matched = gf_filter_pid_caps_match(pid, filter_dst->freg, filter_dst, NULL, NULL, pid->filter->dst_filter, -1);

		if (!cap_matched && filter_dst->clonable) {
			cap_matched  = gf_filter_pid_caps_match(pid, filter_dst->freg, NULL, NULL, NULL, pid->filter->dst_filter, -1);
		}

		if (!cap_matched) {
			Bool reassigned=GF_FALSE;
			u32 j, nb_loaded;
			GF_Filter *new_f, *reuse_f=NULL;

			//we don't load filter chains if we have a change of media type from anything except file to anything except file
			//i.e. transmodality (eg video->audio) can only be done through explicit filters
			if (gf_filter_pid_needs_explicit_resolution(pid, filter_dst))
				continue;

			//we had a destination set during link resolve, and we don't match that filter, consider the link resolution wrong
			if (pid->filter->dst_filter && (filter_dst == pid->filter->dst_filter)) {
				GF_Filter *old_dst = pid->filter->dst_filter;
				pid->filter->dst_filter = NULL;
				//nobody using this filter, destroy
				if (old_dst->dynamic_filter
					&& !old_dst->has_pending_pids
					&& !old_dst->num_input_pids
					&& !old_dst->out_pid_connection_pending
				) {
					assert(!old_dst->finalized);
					old_dst->finalized = GF_TRUE;
					gf_fs_post_task(old_dst->session, gf_filter_remove_task, old_dst, NULL, "filter_destroy", NULL);
				}
			}
			if (first_pass) continue;
			filter_found_but_pid_excluded = GF_FALSE;

			nb_loaded = gf_list_count(loaded_filters);
			for (j=0; j<nb_loaded; j++) {
				u32 out_cap_idx=0;
				reuse_f = gf_list_get(loaded_filters, j);
				//check if we match caps with this loaded filter, in which case assume we can use that filter for connections
				//THIS MIGHT NEED REFINEMENT
				if (gf_filter_pid_caps_match(pid, reuse_f->freg, reuse_f, NULL, &out_cap_idx, pid->filter->dst_filter, -1)) {
					break;
				}
				reuse_f = NULL;
			}
			if (reuse_f) {
				GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Skip link from %s:%s to %s because already connected to filter %s which can handle the connection\n", pid->filter->name, pid->name, filter_dst->name, reuse_f->name));
				continue;
			}

			new_f = gf_filter_pid_resolve_link(pid, filter_dst, &reassigned);
			//try to load filters
			if (! new_f) {
				//filter was reassigned (pid is destroyed), return
				if (reassigned) {
					if (filter->session->filters_mx) gf_mx_v(filter->session->filters_mx);
					assert(pid->init_task_pending);
					safe_int_dec(&pid->init_task_pending);
					if (loaded_filters) gf_list_del(loaded_filters);
					return;
				}
				//we might had it wrong solving the chain initially, break the chain
				if (filter_dst->dynamic_filter && filter_dst->dst_filter) {
					GF_Filter *new_dst = filter_dst;
					while (new_dst->dst_filter && new_dst->dynamic_filter) {
						GF_Filter *f = new_dst;
						new_dst = new_dst->dst_filter;
						if (!f->num_input_pids && !f->num_output_pids && !f->in_pid_connection_pending) {
							f->finalized = GF_TRUE;
							gf_fs_post_task(f->session, gf_filter_remove_task, f, NULL, "filter_destroy", NULL);
						}
					}
					
					pid->filter->dst_filter = NULL;
					new_f = gf_filter_pid_resolve_link(pid, new_dst, &reassigned);
					if (!new_f) {
						if (reassigned) {
							if (filter->session->filters_mx) gf_mx_v(filter->session->filters_mx);
							safe_int_dec(&pid->init_task_pending);
							return;
						} else {
							continue;
						}
					}
					//good to go !
				} else {
					continue;
				}
			}
			filter_dst = new_f;
			gf_list_add(loaded_filters, new_f);
		}
		assert(pid->pid->filter->freg != filter_dst->freg);

		safe_int_inc(&pid->filter->out_pid_connection_pending);
		gf_filter_pid_post_connect_task(filter_dst, pid);

		found_dest = GF_TRUE;
	}
	if (filter->session->filters_mx) gf_mx_v(filter->session->filters_mx);

	if (loaded_filters) gf_list_del(loaded_filters);

	//connection task posted, nothing left to do
	if (found_dest) {
		//we connected ourselves to that filter. In case this is a demux filter, we may have other pids
		//connecting from that filter, so reset the destination to allow for graph resolution
		pid->filter->dst_filter = NULL;
		assert(pid->init_task_pending);
		safe_int_dec(&pid->init_task_pending);
		return;
	}

	//nothing found, redo a pass, this time allowing for link resolve
	if (first_pass && can_try_link_resuolution && filter->session->max_resolve_chain_len) {
		first_pass = GF_FALSE;
		goto restart;
	}
	
	if (filter_found_but_pid_excluded) {
		//PID was not included in explicit connection lists
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("PID %s in filter %s not connected to any loaded filter due to source directives\n", pid->name, pid->filter->name));
	} else {
		GF_FilterEvent evt;

		//no filter found for this pid !
		GF_LOG(pid->not_connected_ok ? GF_LOG_DEBUG : GF_LOG_WARNING, GF_LOG_FILTER, ("No filter chain found for PID %s in filter %s to any loaded filters - NOT CONNECTED\n", pid->name, pid->filter->name));

		if (pid->filter->freg->process_event) {
			GF_FEVT_INIT(evt, GF_FEVT_CONNECT_FAIL, pid);
			pid->filter->freg->process_event(filter, &evt);
		}

		GF_FEVT_INIT(evt, GF_FEVT_PLAY, pid);
		gf_filter_pid_send_event_internal(pid, &evt, GF_TRUE);
		
		GF_FEVT_INIT(evt, GF_FEVT_STOP, pid);
		gf_filter_pid_send_event_internal(pid, &evt, GF_TRUE);

		if (!pid->not_connected_ok && !filter->session->max_resolve_chain_len) {
			filter->session->last_connect_error = GF_FILTER_NOT_FOUND;
		}
		filter->num_out_pids_not_connected ++;
	}
	assert(pid->init_task_pending);
	safe_int_dec(&pid->init_task_pending);
	return;
}

void gf_filter_pid_post_connect_task(GF_Filter *filter, GF_FilterPid *pid)
{
	assert(pid->pid);
	assert(pid->filter != filter);
	assert(pid->filter->freg != filter->freg);
	assert(filter->freg->configure_pid);
	safe_int_inc(&filter->session->pid_connect_tasks_pending);
	safe_int_inc(&filter->in_pid_connection_pending);
	gf_fs_post_task_ex(filter->session, gf_filter_pid_connect_task, filter, pid, "pid_init", NULL, GF_TRUE);
}


void gf_filter_pid_post_init_task(GF_Filter *filter, GF_FilterPid *pid)
{
	if (pid->init_task_pending) return;

	safe_int_inc(&pid->init_task_pending);
	gf_fs_post_task(filter->session, gf_filter_pid_init_task, filter, pid, "pid_init", NULL);
}

void gf_filter_reconnect_output(GF_Filter *filter)
{
	u32 i;
	for (i=0; i<filter->num_output_pids; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		gf_filter_pid_post_init_task(filter, pid);
	}
}

GF_EXPORT
GF_Err gf_filter_pid_set_framing_mode(GF_FilterPid *pid, Bool requires_full_blocks)
{
	GF_FilterPidInst *pidinst = (GF_FilterPidInst *)pid;

	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to set framing info on an output pid in filter %s\n", pid->filter->name));
		return GF_BAD_PARAM;
	}
	pidinst->requires_full_data_block = requires_full_blocks;
	return GF_OK;
}

GF_EXPORT
GF_FilterPid *gf_filter_pid_new(GF_Filter *filter)
{
	char szName[30];
	GF_FilterPid *pid;
	GF_SAFEALLOC(pid, GF_FilterPid);
	pid->filter = filter;
	pid->destinations = gf_list_new();
	pid->properties = gf_list_new();
	if (!filter->output_pids) filter->output_pids = gf_list_new();
	gf_list_add(filter->output_pids, pid);
	filter->num_output_pids = gf_list_count(filter->output_pids);
	pid->pid = pid;
	pid->playback_speed_scaler = GF_FILTER_SPEED_SCALER;
	
	sprintf(szName, "PID%d", filter->num_output_pids);
	pid->name = gf_strdup(szName);

	filter->has_pending_pids = GF_TRUE;
	gf_fq_add(filter->pending_pids, pid);

	//by default copy properties if only one input pid
	if (filter->num_input_pids==1) {
		GF_FilterPid *pidi = gf_list_get(filter->input_pids, 0);
		gf_filter_pid_copy_properties(pid, pidi);
	}
	return pid;
}

void gf_filter_pid_del(GF_FilterPid *pid)
{
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s pid %s destruction\n", pid->filter->name, pid->name));
	while (gf_list_count(pid->destinations)) {
		gf_filter_pid_inst_del( gf_list_pop_back(pid->destinations) );
	}
	gf_list_del(pid->destinations);

	while (gf_list_count(pid->properties)) {
		GF_PropertyMap *prop = gf_list_pop_back(pid->properties);
		if (safe_int_dec(&prop->reference_count)==0) {
			gf_props_del(prop);
		}
	}
	gf_list_del(pid->properties);

	//this one is NOT reference counted
	if(pid->caps_negociate && (safe_int_dec(&pid->caps_negociate->reference_count)==0) )
		gf_props_del(pid->caps_negociate);

	if (pid->adapters_blacklist)
		gf_list_del(pid->adapters_blacklist);

	if (pid->infos) {
		if (safe_int_dec(&pid->infos->reference_count)==0) {
			gf_props_del(pid->infos);
		}
	}
	if (pid->name) gf_free(pid->name);
	gf_free(pid);
}

void gf_filter_pid_del_task(GF_FSTask *task)
{
	gf_filter_pid_del(task->pid);
}

static GF_PropertyMap *check_new_pid_props(GF_FilterPid *pid, Bool merge_props)
{
	u32 i, nb_recf;
	GF_PropertyMap *old_map = gf_list_last(pid->properties);
	GF_PropertyMap *map;

	pid->props_changed_since_connect = GF_TRUE;
	if (old_map && !pid->request_property_map) {
		return old_map;
	}
	pid->request_property_map = GF_FALSE;
	pid->pid_info_changed = GF_FALSE;
	map = gf_props_new(pid->filter);
	if (!map) return NULL;
	gf_list_add(pid->properties, map);

	//when creating a new map, ref_count of old map is decremented
	if (old_map) {
		if (merge_props)
			gf_props_merge_property(map, old_map, NULL, NULL);
		assert(old_map->reference_count);
		if ( safe_int_dec(&old_map->reference_count) == 0) {
			gf_list_del_item(pid->properties, old_map);
			gf_props_del(old_map);
		}
	}

	//trick here: we may be reconfigured before any packet is being dispatched
	//so we need to manually trigger reconfigure of outputs
	nb_recf = 0;
	for (i=0; i<pid->num_destinations; i++) {
		GF_FilterPidInst *pidi = gf_list_get(pid->destinations, i);
		if (!pidi->filter->process_task_queued) {
			//remember the pid prop map to use
			pidi->reconfig_pid_props = map;
			nb_recf++;
		}
	}
	if (nb_recf)
		pid->filter->reconfigure_outputs = GF_TRUE;
	return map;
}

static GF_Err gf_filter_pid_set_property_full(GF_FilterPid *pid, u32 prop_4cc, const char *prop_name, char *dyn_name, const GF_PropertyValue *value, Bool is_info)
{
	GF_PropertyMap *map;

	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to write property on input PID in filter %s - ignoring\n", pid->filter->name));
		return GF_BAD_PARAM;
	}
	//info property, do not request a new property map
	if (is_info) {
		map = pid->infos;
		if (!map) {
			map = pid->infos = gf_props_new(pid->filter);
		}
		pid->pid_info_changed = GF_TRUE;
	} else {
		//always merge properties
		map = check_new_pid_props(pid, GF_TRUE);
	}
	if (!map) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("No properties for destination pid in filter %s, ignoring reset\n", pid->filter->name));
		return GF_OUT_OF_MEM;
	}
	if (prop_4cc==GF_PROP_PID_TIMESCALE) map->timescale = value->value.uint;

	if (value && (prop_4cc == GF_PROP_PID_ID)) {
		char szName[100];
		sprintf(szName, "PID%d", value->value.uint);
		gf_filter_pid_set_name(pid, szName);
	}
	return gf_props_set_property(map, prop_4cc, prop_name, dyn_name, value);
}

GF_EXPORT
GF_Err gf_filter_pid_set_property(GF_FilterPid *pid, u32 prop_4cc, const GF_PropertyValue *value)
{
	if (!prop_4cc) return GF_BAD_PARAM;
	return gf_filter_pid_set_property_full(pid, prop_4cc, NULL, NULL, value, GF_FALSE);
}

GF_EXPORT
GF_Err gf_filter_pid_set_property_str(GF_FilterPid *pid, const char *name, const GF_PropertyValue *value)
{
	if (!name) return GF_BAD_PARAM;
	return gf_filter_pid_set_property_full(pid, 0, name, NULL, value, GF_FALSE);
}

GF_EXPORT
GF_Err gf_filter_pid_set_property_dyn(GF_FilterPid *pid, char *name, const GF_PropertyValue *value)
{
	if (!name) return GF_BAD_PARAM;
	return gf_filter_pid_set_property_full(pid, 0, NULL, name, value, GF_FALSE);
}

GF_EXPORT
GF_Err gf_filter_pid_set_info(GF_FilterPid *pid, u32 prop_4cc, const GF_PropertyValue *value)
{
	if (!prop_4cc) return GF_BAD_PARAM;
	return gf_filter_pid_set_property_full(pid, prop_4cc, NULL, NULL, value, GF_TRUE);
}

GF_EXPORT
GF_Err gf_filter_pid_set_info_str(GF_FilterPid *pid, const char *name, const GF_PropertyValue *value)
{
	if (!name) return GF_BAD_PARAM;
	return gf_filter_pid_set_property_full(pid, 0, name, NULL, value, GF_TRUE);
}

GF_EXPORT
GF_Err gf_filter_pid_set_info_dyn(GF_FilterPid *pid, char *name, const GF_PropertyValue *value)
{
	if (!name) return GF_BAD_PARAM;
	return gf_filter_pid_set_property_full(pid, 0, NULL, name, value, GF_TRUE);
}

static GF_Err gf_filter_pid_negociate_property_full(GF_FilterPid *pid, u32 prop_4cc, const char *prop_name, char *dyn_name, const GF_PropertyValue *value)
{
	GF_FilterPidInst *pidi = (GF_FilterPidInst *) pid;
	if (!prop_4cc) return GF_BAD_PARAM;

	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to negociate property on output PID in filter %s - ignoring\n", pid->filter->name));
		return GF_BAD_PARAM;
	}
	pid = pid->pid;
	if (!pid->caps_negociate) {
		assert(!pid->caps_negociate_pidi);
		pid->caps_negociate = gf_props_new(pid->filter);
		pid->caps_negociate_pidi = pidi;
		//we start a new caps negotiation step, reset any blacklist on pid
		if (pid->adapters_blacklist) {
			gf_list_del(pid->adapters_blacklist);
			pid->adapters_blacklist = NULL;
		}
		safe_int_inc(&pid->filter->nb_caps_renegociate);
	}
	return gf_props_set_property(pid->caps_negociate, prop_4cc, prop_name, dyn_name, value);
}

GF_EXPORT
GF_Err gf_filter_pid_negociate_property(GF_FilterPid *pid, u32 prop_4cc, const GF_PropertyValue *value)
{
	if (!prop_4cc) return GF_BAD_PARAM;
	return gf_filter_pid_negociate_property_full(pid, prop_4cc, NULL, NULL, value);
}

GF_EXPORT
GF_Err gf_filter_pid_negociate_property_str(GF_FilterPid *pid, const char *name, const GF_PropertyValue *value)
{
	if (!name) return GF_BAD_PARAM;
	return gf_filter_pid_negociate_property_full(pid, 0, name, NULL, value);
}

GF_EXPORT
GF_Err gf_filter_pid_negociate_property_dyn(GF_FilterPid *pid, char *name, const GF_PropertyValue *value)
{
	if (!name) return GF_BAD_PARAM;
	return gf_filter_pid_negociate_property_full(pid, 0, NULL, name, value);
}


static GF_PropertyMap *filter_pid_get_prop_map(GF_FilterPid *pid)
{
	if (PID_IS_INPUT(pid)) {
		GF_FilterPidInst *pidi = (GF_FilterPidInst *) pid;
		//first time we access the props, use the first entry in the property list
		if (!pidi->props) {
			pidi->props = gf_list_get(pid->pid->properties, 0);
			assert(pidi->props);
			safe_int_inc(&pidi->props->reference_count);
		}
		return pidi->props;
	} else {
		pid = pid->pid;
		return gf_list_last(pid->properties);
	}
	return NULL;
}

GF_EXPORT
const GF_PropertyValue *gf_filter_pid_get_property(GF_FilterPid *pid, u32 prop_4cc)
{
	GF_PropertyMap *map = filter_pid_get_prop_map(pid);
	if (!map)
		return NULL;
	return gf_props_get_property(map, prop_4cc, NULL);
}

GF_EXPORT
const GF_PropertyValue *gf_filter_pid_get_property_str(GF_FilterPid *pid, const char *prop_name)
{
	GF_PropertyMap *map = filter_pid_get_prop_map(pid);
	if (!map)
		return NULL;
	return gf_props_get_property(map, 0, prop_name);
}

static const GF_PropertyValue *gf_filter_pid_get_info_internal(GF_FilterPid *pid, u32 prop_4cc, const char *prop_name)
{
	u32 i, count;
	const GF_PropertyValue * prop;
	GF_PropertyMap *map = filter_pid_get_prop_map(pid);
	if (map) {
		prop = gf_props_get_property(map, prop_4cc, prop_name);
		if (prop) return prop;
	}
	if (PID_IS_OUTPUT(pid)) {
		return NULL;
	}
	pid = pid->pid;
	if (pid->infos) {
		prop = gf_props_get_property(pid->infos, prop_4cc, prop_name);
		if (prop) return prop;
	}

	count = gf_list_count(pid->filter->input_pids);
	for (i=0; i<count; i++) {
		GF_FilterPid *pidinst = gf_list_get(pid->filter->input_pids, i);
		prop = gf_filter_pid_get_info_internal(pidinst->pid, prop_4cc, prop_name);
		if (prop) return prop;
	}
	return NULL;
}

GF_EXPORT
const GF_PropertyValue *gf_filter_pid_get_info(GF_FilterPid *pid, u32 prop_4cc)
{
	return gf_filter_pid_get_info_internal(pid, prop_4cc, NULL);
}

GF_EXPORT
const GF_PropertyValue *gf_filter_pid_get_info_str(GF_FilterPid *pid, const char *prop_name)
{
	return gf_filter_pid_get_info_internal(pid, 0, prop_name);
}

static const GF_PropertyValue *gf_filter_get_info_internal(GF_Filter *filter, u32 prop_4cc, const char *prop_name)
{
	u32 i, count;
	const GF_PropertyValue * prop;

	//TODO avoid doing back and forth ...
	count = gf_list_count(filter->output_pids);
	for (i=0; i<count; i++) {
		GF_FilterPid *pid = gf_list_get(filter->output_pids, i);
		prop = gf_filter_pid_get_info_internal(pid, prop_4cc, prop_name);
		if (prop) return prop;
	}
	count = gf_list_count(filter->input_pids);
	for (i=0; i<count; i++) {
		GF_FilterPidInst *pidinst = gf_list_get(filter->input_pids, i);
		prop = gf_filter_pid_get_info_internal(pidinst->pid, prop_4cc, prop_name);
		if (prop) return prop;
	}
	return NULL;
}

GF_EXPORT
const GF_PropertyValue *gf_filter_get_info(GF_Filter *filter, u32 prop_4cc)
{
	return gf_filter_get_info_internal(filter, prop_4cc, NULL);
}

GF_EXPORT
const GF_PropertyValue *gf_filter_get_info_str(GF_Filter *filter, const char *prop_name)
{
	return gf_filter_get_info_internal(filter, 0, prop_name);
}

GF_Err gf_filter_pid_reset_properties(GF_FilterPid *pid)
{
	GF_PropertyMap *map;

	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to reset all properties on input PID in filter %s - ignoring\n", pid->filter->name));
		return GF_BAD_PARAM;
	}
	//don't merge properties, we will reset them anyway
	map = check_new_pid_props(pid, GF_FALSE);

	if (!map) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("No properties for destination pid in filter %s, ignoring reset\n", pid->filter->name));
		return GF_OUT_OF_MEM;
	}
	gf_props_reset(map);
	return GF_OK;

}

GF_EXPORT
GF_Err gf_filter_pid_copy_properties(GF_FilterPid *dst_pid, GF_FilterPid *src_pid)
{
	GF_PropertyMap *dst_props, *src_props;

	if (PID_IS_INPUT(dst_pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to reset all properties on input PID in filter %s - ignoring\n", dst_pid->filter->name));
		return GF_BAD_PARAM;
	}
	//don't merge properties with old state we merge with source pid
	dst_props = check_new_pid_props(dst_pid, GF_FALSE);

	if (!dst_props) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("No properties for destination pid in filter %s, ignoring reset\n", dst_pid->filter->name));
		return GF_OUT_OF_MEM;
	}
	src_pid = src_pid->pid;
	src_props = gf_list_last(src_pid->properties);
	if (!src_props) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("No properties for source pid in filter %s, ignoring merge\n", src_pid->filter->name));
		return GF_OK;
	}
	if (src_pid->name) gf_filter_pid_set_name(dst_pid, src_pid->name);
	
	gf_props_reset(dst_props);
	return gf_props_merge_property(dst_props, src_props, NULL, NULL);
}

GF_EXPORT
u32 gf_filter_pid_get_packet_count(GF_FilterPid *pid)
{
	GF_FilterPidInst *pidinst = (GF_FilterPidInst *)pid;
	if (PID_IS_OUTPUT(pid)) {
		pidinst = gf_list_get(pid->destinations, 0);
		if (! pidinst) return 0;
		return gf_fq_count(pidinst->packets) - pidinst->nb_eos_signaled - pidinst->nb_clocks_signaled;

	} else {
		if (pidinst->discard_packets) return 0;
		return gf_fq_count(pidinst->packets) - pidinst->nb_eos_signaled - pidinst->nb_clocks_signaled;
	}
}

static Bool gf_filter_pid_filter_internal_packet(GF_FilterPidInst *pidi, GF_FilterPacketInstance *pcki)
{
	Bool is_internal = GF_FALSE;
	u32 ctype = (pcki->pck->info.flags & GF_PCK_CMD_MASK);
	if (ctype == GF_PCK_CMD_PID_EOS ) {
		pcki->pid->is_end_of_stream = pcki->pid->pid->has_seen_eos ? GF_TRUE : GF_FALSE;
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Found EOS packet in PID %s in filter %s - eos %d\n", pidi->pid->name, pidi->filter->name, pcki->pid->pid->has_seen_eos));
		safe_int_dec(&pcki->pid->nb_eos_signaled);
		is_internal = GF_TRUE;
	} else if (ctype == GF_PCK_CMD_PID_REM) {
		gf_fs_post_task(pidi->filter->session, gf_filter_pid_disconnect_task, pidi->filter, pidi->pid, "pidinst_disconnect", NULL);

		is_internal = GF_TRUE;
	}
	ctype = (pcki->pck->info.flags & GF_PCK_CKTYPE_MASK) >> GF_PCK_CKTYPE_POS;

	if (ctype) {
		if (pcki->pid->handles_clock_references) return GF_FALSE;
		safe_int_dec(&pcki->pid->nb_clocks_signaled);
		//signal destination
		assert(!pcki->pid->filter->next_clock_dispatch_type || !pcki->pid->filter->num_output_pids);

		pcki->pid->filter->next_clock_dispatch = pcki->pck->info.cts;
		pcki->pid->filter->next_clock_dispatch_timescale = pcki->pck->pid_props->timescale;
		pcki->pid->filter->next_clock_dispatch_type = ctype;

		//keep clock values but only override clock type if no discontinuity is pending
		pcki->pid->last_clock_value = pcki->pck->info.cts;
		pcki->pid->last_clock_timescale = pcki->pck->pid_props->timescale;
		if (pcki->pid->last_clock_type != GF_FILTER_CLOCK_PCR_DISC)
			pcki->pid->last_clock_type = ctype;

		if (ctype == GF_FILTER_CLOCK_PCR_DISC) {
			assert(pcki->pid->last_clock_type == GF_FILTER_CLOCK_PCR_DISC);
		}
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Internal clock reference packet filtered - PID %s clock ref "LLU"/%d - type %d\n", pcki->pid->pid->name, pcki->pid->last_clock_value, pcki->pid->last_clock_timescale, pcki->pid->last_clock_type));
		//the following call to drop_packet will trigger clock forwarding to all output pids
		is_internal = GF_TRUE;
	}

	if (is_internal) gf_filter_pid_drop_packet((GF_FilterPid *)pidi);
	return is_internal;
}

GF_EXPORT
GF_FilterPacket *gf_filter_pid_get_packet(GF_FilterPid *pid)
{
	GF_FilterPacketInstance *pcki;
	GF_FilterPidInst *pidinst = (GF_FilterPidInst *)pid;
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to fetch a packet on an output PID in filter %s\n", pid->filter->name));
		return NULL;
	}
	if (pidinst->discard_packets) return NULL;
	if (pidinst->detach_pending) return NULL;

	pcki = (GF_FilterPacketInstance *)gf_fq_head(pidinst->packets);
	//no packets
	if (!pcki) {
		if (pidinst->pid->filter->force_end_of_session) {
			pidinst->is_end_of_stream = pidinst->pid->has_seen_eos = GF_TRUE;
		}
		if (!pidinst->is_end_of_stream && pidinst->pid->filter->would_block)
			gf_filter_pid_check_unblock(pidinst->pid);
		return NULL;
	}
	assert(pcki->pck);

	if (gf_filter_pid_filter_internal_packet(pidinst, pcki))  {
		return gf_filter_pid_get_packet(pid);
	}
	pcki->pid->is_end_of_stream = GF_FALSE;

	if ( (pcki->pck->info.flags & GF_PCKF_PROPS_CHANGED) && !pcki->pid_props_change_done) {
		GF_Err e;
		Bool skip_props = GF_FALSE;

		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s PID %s property changed at this packet, triggering reconfigure\n", pidinst->pid->filter->name, pidinst->pid->name));
		pcki->pid_props_change_done = 1;

		//it may happen that:
		//- the props are not set when querying the first packet (no prop queries on pid)
		//- the new props are already set if filter_pid_get_property was queried before the first packet dispatch
		if (pidinst->props) {
			if (pidinst->props != pcki->pck->pid_props) {
				//unassign old property list and set the new one
				if (safe_int_dec(& pidinst->props->reference_count) == 0) {
					gf_list_del_item(pidinst->pid->properties, pidinst->props);
					gf_props_del(pidinst->props);
				}
				pidinst->props = pcki->pck->pid_props;
				safe_int_inc( & pidinst->props->reference_count );
			} else {
				//it may happen that pid_configure for destination was called after packet being dispatched, in
				//which case we are already properly configured
				skip_props = GF_TRUE;
			}
		}
		if (!skip_props) {
			assert(pidinst->filter->freg->configure_pid);
			//reset the blacklist whenever reconfiguring, since we may need to reload a new filter chain
			//in which a previously blacklisted filter (failing (re)configure for previous state) could
			//now work, eg moving from formatA to formatB then back to formatA
			gf_list_reset(pidinst->filter->blacklisted);

			e = gf_filter_pid_configure(pidinst->filter, pidinst->pid, GF_PID_CONF_RECONFIG);
			if (e != GF_OK) return NULL;
			if (pidinst->pid->caps_negociate)
				return NULL;
		}
	}
	if ( (pcki->pck->info.flags & GF_PCKF_INFO_CHANGED) /*&& !pcki->pid_info_change_done*/) {
		Bool res=GF_FALSE;

		//it may happen that this filter pid is pulled from another thread than ours (eg audio callback), in which case
		//we cannot go reentrant, we have to wait until the filter is not in use ...
		if (pidinst->filter->freg->process_event && pidinst->filter->process_th_id && (pidinst->filter->process_th_id != gf_th_id()) ) {
			return NULL;
		}
		pcki->pid_info_change_done = 1;

		if (pidinst->filter->freg->process_event) {
			GF_FilterEvent evt;
			GF_FEVT_INIT(evt, GF_FEVT_INFO_UPDATE, pid);

			FSESS_CHECK_THREAD(pidinst->filter)
			res = pidinst->filter->freg->process_event(pidinst->filter, &evt);
		}
		
		if (!res) {
			pidinst->filter->pid_info_changed = GF_TRUE;
		}
	}
	pidinst->last_pck_fetch_time = gf_sys_clock_high_res();

	return (GF_FilterPacket *)pcki;
}

GF_EXPORT
Bool gf_filter_pid_get_first_packet_cts(GF_FilterPid *pid, u64 *cts)
{
	GF_FilterPacketInstance *pcki;
	GF_FilterPidInst *pidinst = (GF_FilterPidInst *)pid;
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to read packet CTS on an output PID in filter %s\n", pid->filter->name));
		return GF_FALSE;
	}
	if (pidinst->discard_packets) return GF_FALSE;

	pcki = (GF_FilterPacketInstance *)gf_fq_head(pidinst->packets);
	//no packets
	if (!pcki) {
		return GF_FALSE;
	}
	assert(pcki->pck);

	if (gf_filter_pid_filter_internal_packet(pidinst, pcki))  {
		return gf_filter_pid_get_first_packet_cts(pid, cts);
	}

	if (pidinst->requires_full_data_block && !(pcki->pck->info.flags & GF_PCKF_BLOCK_END))
		return GF_FALSE;
	*cts = pcki->pck->info.cts;
	return GF_TRUE;
}

GF_EXPORT
Bool gf_filter_pid_first_packet_is_empty(GF_FilterPid *pid)
{
	GF_FilterPacketInstance *pcki;
	GF_FilterPidInst *pidinst = (GF_FilterPidInst *)pid;
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to read packet CTS on an output PID in filter %s\n", pid->filter->name));
		return GF_TRUE;
	}
	if (pidinst->discard_packets) return GF_TRUE;

	pcki = (GF_FilterPacketInstance *)gf_fq_head(pidinst->packets);
	//no packets
	if (!pcki) {
		return GF_TRUE;
	}
	assert(pcki->pck);

	if (pcki->pck->info.flags & (GF_PCK_CMD_MASK|GF_PCK_CKTYPE_MASK)) {
		return GF_TRUE;
	}
	if (pidinst->requires_full_data_block && !(pcki->pck->info.flags & GF_PCKF_BLOCK_END))
		return GF_TRUE;
	return (pcki->pck->data_length || pcki->pck->hw_frame) ? GF_FALSE : GF_TRUE;
}


static void gf_filter_pidinst_update_stats(GF_FilterPidInst *pidi, GF_FilterPacket *pck)
{
	u64 now = gf_sys_clock_high_res();
	u64 dec_time = now - pidi->last_pck_fetch_time;
	if (pck->info.flags & GF_PCK_CMD_MASK) return;
	if (pidi->pid->filter->removed) return;

	pidi->filter->nb_pck_processed++;
	pidi->filter->nb_bytes_processed += pck->data_length;

	pidi->total_process_time += dec_time;
	if (!pidi->nb_processed) {
		pidi->first_frame_time = pidi->last_pck_fetch_time;
	}

	pidi->nb_processed++;
	if (pck->info.flags & GF_PCK_SAP_MASK) {
		pidi->nb_sap_processed ++;
		if (dec_time > pidi->max_sap_process_time) pidi->max_sap_process_time = dec_time;
		pidi->total_sap_process_time += dec_time;
	}

	if (dec_time > pidi->max_process_time) pidi->max_process_time = dec_time;

	if (pck->data_length) {
		Bool has_ts = GF_TRUE;
		u64 ts = (pck->info.dts != GF_FILTER_NO_TS) ? pck->info.dts : pck->info.cts;
		if ((ts != GF_FILTER_NO_TS) && (pck->pid_props->timescale)) {
			ts *= 1000000;
			ts /= pck->pid_props->timescale;
		} else {
			has_ts = GF_FALSE;
		}
		
		if (!pidi->cur_bit_size) {
			pidi->stats_start_ts = ts;
			pidi->stats_start_us = now;
			pidi->cur_bit_size = 8*pck->data_length;
		} else {
			Bool flush_stats = GF_FALSE;
			pidi->cur_bit_size += 8*pck->data_length;

			if (has_ts) {
				if (pidi->stats_start_ts + 1000000 <= ts) flush_stats = GF_TRUE;
			} else {
				if (pidi->stats_start_us + 1000000 <= now) flush_stats = GF_TRUE;
			}

			if (flush_stats) {
				u64 rate;
				if (has_ts) {
					rate = pidi->cur_bit_size;
					rate *= 1000000;
					rate /= (ts - pidi->stats_start_ts);
					pidi->avg_bit_rate = (u32) rate;
					if (pidi->avg_bit_rate > pidi->max_bit_rate) pidi->max_bit_rate = pidi->avg_bit_rate;
				}

				rate = pidi->cur_bit_size;
				rate *= 1000000;
				rate /= (now - pidi->stats_start_us);
				pidi->avg_process_rate = (u32) rate;
				if (pidi->avg_process_rate > pidi->max_process_rate) pidi->max_process_rate = pidi->avg_process_rate;

				//reset stats
				pidi->cur_bit_size = 0;
			}
		}
	}
}

static void gf_filter_pidinst_reset_stats(GF_FilterPidInst *pidi)
{
	pidi->last_pck_fetch_time = 0;
	pidi->stats_start_ts = 0;
	pidi->stats_start_us = 0;
	pidi->cur_bit_size = 0;
	pidi->avg_bit_rate = 0;
	pidi->max_bit_rate = 0;
	pidi->avg_process_rate = 0;
	pidi->max_process_rate = 0;
	pidi->nb_processed = 0;
	pidi->nb_sap_processed = 0;
	pidi->total_process_time = 0;
	pidi->total_sap_process_time = 0;
	pidi->max_process_time = 0;
	pidi->max_sap_process_time = 0;
	pidi->first_frame_time = 0;
}

GF_EXPORT
void gf_filter_pid_drop_packet(GF_FilterPid *pid)
{
	u32 nb_pck=0;
	GF_FilterPacket *pck=NULL;
	GF_FilterPacketInstance *pcki;
	GF_FilterPidInst *pidinst = (GF_FilterPidInst *)pid;

	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to discard a packet on an output PID in filter %s\n", pid->filter->name));
		return;
	}
	//remove pck instance
	pcki = gf_fq_pop(pidinst->packets);

	if (!pcki) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("Attempt to discard a packet already discarded in filter %s\n", pid->filter->name));
		return;
	}

	gf_rmt_begin(pck_drop, GF_RMT_AGGREGATE);
	pck = pcki->pck;
	//move to source pid
	pid = pid->pid;

	nb_pck=gf_fq_count(pidinst->packets);

	gf_filter_pidinst_update_stats(pidinst, pck);

	gf_mx_p(pid->filter->tasks_mx);

	if (nb_pck<pid->nb_buffer_unit) {
		pid->nb_buffer_unit = nb_pck;
	}

	if (!nb_pck) {
		safe_int64_sub(&pidinst->buffer_duration, pidinst->buffer_duration);
	} else if (pck->info.duration && (pck->info.flags & GF_PCKF_BLOCK_START)  && pck->pid_props->timescale) {
		s64 d = ((u64)pck->info.duration) * 1000000;
		d /= pck->pid_props->timescale;
		if (d > pidinst->buffer_duration) {
			GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Corrupted buffer level in PID instance %s (%s -> %s), droping packet duration "LLD" us greater than buffer duration "LLU" us\n", pid->name, pid->filter->name, pidinst->filter ? pidinst->filter->name : "disconnected", d, pidinst->buffer_duration));
			d = pidinst->buffer_duration;
		}
		assert(d <= pidinst->buffer_duration);
		safe_int64_sub(&pidinst->buffer_duration, (s32) d);
		assert(pidinst->buffer_duration != 39000);
	}

	if (!pid->buffer_duration || (pidinst->buffer_duration < (s64) pid->buffer_duration)) {
		pid->buffer_duration = pidinst->buffer_duration;
	}
	gf_filter_pid_check_unblock(pid);

	gf_mx_v(pid->filter->tasks_mx);

#ifndef GPAC_DISABLE_LOG
	if (gf_log_tool_level_on(GF_LOG_FILTER, GF_LOG_DEBUG)) {
		u8 sap_type = (pck->info.flags & GF_PCK_SAP_MASK) >> GF_PCK_SAP_POS;
		Bool seek_flag = pck->info.flags & GF_PCKF_SEEK;

		if ((pck->info.dts != GF_FILTER_NO_TS) && (pck->info.cts != GF_FILTER_NO_TS) ) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s PID %s (%s) drop packet DTS "LLU" CTS "LLU" SAP %d Seek %d - %d packets remaining buffer "LLU" us\n", pidinst->filter ? pidinst->filter->name : "disconnected", pid->name, pid->filter->name, pck->info.dts, pck->info.cts, sap_type, seek_flag, nb_pck, pidinst->buffer_duration));
		} else if ((pck->info.cts != GF_FILTER_NO_TS) ) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s PID %s (%s) drop packet CTS "LLU" SAP %d Seek %d - %d packets remaining buffer "LLU" us\n", pidinst->filter ? pidinst->filter->name : "disconnected", pid->name, pid->filter->name, pck->info.cts, sap_type, seek_flag, nb_pck, pidinst->buffer_duration));
		} else if ((pck->info.dts != GF_FILTER_NO_TS) ) {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s PID %s (%s) drop packet DTS "LLU" SAP %d Seek %d - %d packets remaining buffer "LLU" us\n", pidinst->filter ? pidinst->filter->name : "disconnected", pid->name, pid->filter->name, pck->info.dts, sap_type, seek_flag, nb_pck, pidinst->buffer_duration));
		} else {
			GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s PID %s (%s) drop packet SAP %d Seek %d - %d packets remaining buffer "LLU" us\n", pidinst->filter ? pidinst->filter->name : "disconnected", pid->name, pid->filter->name, sap_type, seek_flag, nb_pck, pidinst->buffer_duration));
		}
	}
#endif

	//destroy pcki
	pcki->pck = NULL;
	pcki->pid = NULL;

	gf_fq_add(pid->filter->pcks_inst_reservoir, pcki);

	//unref pck
	if (safe_int_dec(&pck->reference_count) == 0) {
		gf_filter_packet_destroy(pck);
	}
	//decrement number of pending packet on target filter if this is not a destroy
	if (pidinst->filter)
		safe_int_dec(&pidinst->filter->pending_packets);

	if (pidinst->filter)
		gf_filter_forward_clock(pidinst->filter);

	gf_rmt_end();
}

GF_EXPORT
Bool gf_filter_pid_is_eos(GF_FilterPid *pid)
{
	GF_FilterPacketInstance *pcki;
	GF_FilterPidInst *pidi = (GF_FilterPidInst *)pid;

	if (pidi->detach_pending)
		return GF_FALSE;
		
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to query EOS on output PID %s in filter %s\n", pid->pid->name, pid->filter->name));
		return GF_FALSE;
	}
	if (!pid->pid->has_seen_eos) {
		((GF_FilterPidInst *)pid)->is_end_of_stream = GF_FALSE;
		return GF_FALSE;
	}
	//peek next for eos
	pcki = (GF_FilterPacketInstance *)gf_fq_head(pidi->packets);
	if (pcki)
		gf_filter_pid_filter_internal_packet(pidi, pcki);

	return pidi->is_end_of_stream;
}

GF_EXPORT
void gf_filter_pid_set_eos(GF_FilterPid *pid)
{
	GF_FilterPacket *pck;
	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to signal EOS on input PID %s in filter %s\n", pid->pid->name, pid->filter->name));
		return;
	}
	if (pid->pid->has_seen_eos) return;

	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("EOS signaled on PID %s in filter %s\n", pid->name, pid->filter->name));
	//we create a fake packet for eos signaling
	pck = gf_filter_pck_new_shared_internal(pid, NULL, 0, NULL, GF_TRUE);
	gf_filter_pck_set_framing(pck, GF_TRUE, GF_TRUE);
	pck->pck->info.flags |= GF_PCK_CMD_PID_EOS;
	gf_filter_pck_send(pck);
}

GF_EXPORT
const GF_PropertyValue *gf_filter_pid_enum_properties(GF_FilterPid *pid, u32 *idx, u32 *prop_4cc, const char **prop_name)
{
	GF_PropertyMap *props;

	if (PID_IS_INPUT(pid)) {
		props = gf_list_last(pid->pid->properties);
	} else {
		props = check_new_pid_props(pid, GF_FALSE);
	}
	if (!props) {
		GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("No properties for pid in filter %s, ignoring enum\n", pid->filter->name));
		*idx = 0xFFFFFFFF;
		return NULL;
	}
	return gf_props_enum_property(props, idx, prop_4cc, prop_name);
}

GF_EXPORT
Bool gf_filter_pid_would_block(GF_FilterPid *pid)
{
	Bool would_block=GF_FALSE;

	if (PID_IS_INPUT(pid)) {
		return GF_FALSE;
	}

	if (pid->filter->session->disable_blocking)
		return GF_FALSE;

	gf_mx_p(pid->filter->tasks_mx);
	//either block according to the number of dispatched units (decoder output) or to the requested buffer duration
	if (pid->max_buffer_unit) {
		if (pid->nb_buffer_unit * GF_FILTER_SPEED_SCALER >= pid->max_buffer_unit * pid->playback_speed_scaler) {
			would_block = GF_TRUE;
		}
	} else if (pid->max_buffer_time && (pid->buffer_duration * GF_FILTER_SPEED_SCALER > pid->max_buffer_time * pid->playback_speed_scaler) ) {
		would_block = GF_TRUE;
	}
	if (would_block && !pid->would_block) {
		safe_int_inc(&pid->filter->would_block);
		safe_int_inc(&pid->would_block);

#ifndef GPAC_DISABLE_LOG
		if (gf_log_tool_level_on(GF_LOG_FILTER, GF_LOG_DEBUG)) {
			if (pid->max_buffer_unit) {
				GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s PID %s blocked (%d units vs %d max units) - %d filter PIDs blocked\n", pid->pid->filter->name, pid->pid->name, pid->nb_buffer_unit, pid->max_buffer_unit, pid->filter->would_block));
			} else {
				GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter %s PID %s blocked ("LLU" us vs "LLU" max buffer) - %d filter PIDs blocked\n", pid->pid->filter->name, pid->pid->name, pid->buffer_duration, pid->max_buffer_time, pid->filter->would_block));
			}
		}
#endif
	}
	assert(pid->filter->would_block <= pid->filter->num_output_pids);
	gf_mx_v(pid->filter->tasks_mx);
	return would_block;
}

GF_EXPORT
u64 gf_filter_pid_query_buffer_duration(GF_FilterPid *pid, Bool check_decoder_output)
{
	u32 count, i, j;
	u64 duration=0;
	if (PID_IS_INPUT(pid)) {
		GF_Filter *filter;
		GF_FilterPidInst *pidinst = (GF_FilterPidInst *)pid;
		filter = pidinst->pid->filter;
		if (check_decoder_output && pidinst->pid->max_buffer_unit && (pidinst->pid->max_buffer_unit>pidinst->pid->nb_buffer_unit))
			return 0;
		count = filter->num_input_pids;
		for (i=0; i<count; i++) {
			u64 dur = gf_filter_pid_query_buffer_duration( gf_list_get(filter->input_pids, i), check_decoder_output);
			if (dur > duration)
				duration = dur;
		}
		duration += pidinst->buffer_duration;
		return duration;
	} else {
		u32 count2;
		u64 max_dur=0;
		if (check_decoder_output && pid->max_buffer_unit && (pid->max_buffer_unit > pid->nb_buffer_unit))
			return 0;
		count = pid->num_destinations;
		for (i=0; i<count; i++) {
			GF_FilterPidInst *pidinst = gf_list_get(pid->destinations, i);

			count2 = pidinst->filter->num_output_pids;
			for (j=0; j<count2; j++) {
				GF_FilterPid *pid_n = gf_list_get(pidinst->filter->output_pids, i);
				u64 dur = gf_filter_pid_query_buffer_duration(pid_n, check_decoder_output);
				if (dur > max_dur ) max_dur = dur;
			}
		}
		duration += max_dur;
	}
	return duration;
}


GF_EXPORT
Bool gf_filter_pid_has_seen_eos(GF_FilterPid *pid)
{
	u32 i;
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to query EOS on output PID %s in filter %s\n", pid->pid->name, pid->filter->name));
		return GF_FALSE;
	}
	if (pid->pid->has_seen_eos) return GF_TRUE;

	for (i=0; i<pid->pid->filter->num_input_pids; i++) {
		GF_FilterPidInst *pidi = gf_list_get(pid->pid->filter->input_pids, i);
		if (gf_filter_pid_has_seen_eos((GF_FilterPid *) pidi)) return GF_TRUE;
	}
	return GF_FALSE;
}

GF_EXPORT
const char *gf_filter_event_name(GF_FEventType type)
{
	switch (type) {
	case GF_FEVT_PLAY: return "PLAY";
	case GF_FEVT_SET_SPEED: return "SET_SPEED";
	case GF_FEVT_STOP: return "STOP";
	case GF_FEVT_SOURCE_SEEK: return "SOURCE_SEEK";
	case GF_FEVT_SOURCE_SWITCH: return "SOURCE_SWITCH";
	case GF_FEVT_ATTACH_SCENE: return "ATTACH_SCENE";
	case GF_FEVT_RESET_SCENE: return "RESET_SCENE";
	case GF_FEVT_PAUSE: return "PAUSE";
	case GF_FEVT_RESUME: return "RESUME";
	case GF_FEVT_QUALITY_SWITCH: return "QUALITY_SWITCH";
	case GF_FEVT_VISIBILITY_HINT: return "VISIBILITY_HINT";
	case GF_FEVT_INFO_UPDATE: return "INFO_UPDATE";
	case GF_FEVT_BUFFER_REQ: return "BUFFER_REQ";
	case GF_FEVT_MOUSE: return "MOUSE";
	case GF_FEVT_SEGMENT_SIZE: return "SEGMENT_SIZE";
	case GF_FEVT_CAPS_CHANGE: return "CAPS_CHANGED";
	case GF_FEVT_CONNECT_FAIL: return "CONNECT_FAIL";
	default:
		return "UNKNOWN";
	}
}

static void gf_filter_pid_reset_task(GF_FSTask *task)
{
	GF_FilterPidInst *pidi = (GF_FilterPidInst *)task->udta;
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s input PID %s (from %s) reseting buffer\n", task->filter->name, pidi->pid->name, pidi->pid->filter->name ));

	while (gf_fq_count(pidi->packets)) {
		gf_filter_pid_drop_packet((GF_FilterPid *) pidi);
	}
	while (gf_list_count(pidi->pck_reassembly)) {
		GF_FilterPacketInstance *pcki = gf_list_pop_back(pidi->pck_reassembly);
		pcki_del(pcki);
	}
	gf_filter_pidinst_reset_stats(pidi);

	pidi->discard_packets = GF_FALSE;
	pidi->last_block_ended = GF_TRUE;
	pidi->first_block_started = GF_FALSE;
	pidi->is_end_of_stream = GF_FALSE;
	pidi->buffer_duration = 0;
	pidi->nb_eos_signaled = 0;
	pidi->pid->has_seen_eos = GF_FALSE;

	safe_int_dec(& pidi->pid->filter->stream_reset_pending );

	pidi->pid->nb_buffer_unit = 0;
	pidi->pid->nb_buffer_unit = 0;

	assert(pidi->pid->discard_input_packets);
	safe_int_dec(& pidi->pid->discard_input_packets );
}

void gf_filter_pid_send_event_downstream(GF_FSTask *task)
{
	u32 i, count;
	Bool canceled = GF_FALSE;
	GF_FilterEvent *evt = task->udta;
	GF_Filter *f = task->filter;
	GF_List *dispatched_filters = NULL;

	//if stream reset task is posted, wait for it before processing this event
	if (f->stream_reset_pending) {
		TASK_REQUEUE(task)
		return;
	}
	//if some pids are still detached, wait for the connection before processing this event
	if (f->detached_pid_inst) {
		TASK_REQUEUE(task)
		return;
	}

	if (evt->base.on_pid) {
		assert(evt->base.on_pid->filter->num_events_queued);
		safe_int_dec(&evt->base.on_pid->filter->num_events_queued);
	}

	if (evt->base.type == GF_FEVT_BUFFER_REQ) {
		if (!evt->base.on_pid) {
			gf_free(evt);
			return;
		}
		if (evt->base.on_pid->nb_decoder_inputs || evt->base.on_pid->raw_media || evt->buffer_req.pid_only) {
			evt->base.on_pid->max_buffer_time = evt->base.on_pid->user_max_buffer_time = evt->buffer_req.max_buffer_us;
			evt->base.on_pid->user_max_playout_time = evt->buffer_req.max_playout_us;
			evt->base.on_pid->max_buffer_unit = 0;
			
			//update blocking state
			if (evt->base.on_pid->would_block)
				gf_filter_pid_check_unblock(evt->base.on_pid);
			else
				gf_filter_pid_would_block(evt->base.on_pid);
			canceled = GF_TRUE;
		}
	} else if (evt->base.on_pid && (evt->base.type == GF_FEVT_PLAY) && evt->base.on_pid->pid->is_playing) {
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s PID %s event %s but PID is already playing, discarding\n", f->name, evt->base.on_pid ? evt->base.on_pid->name : "none", gf_filter_event_name(evt->base.type)));
		gf_free(evt);
		return;
	} else if (evt->base.on_pid && (evt->base.type == GF_FEVT_STOP) && !evt->base.on_pid->pid->is_playing) {
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s PID %s event %s but PID is not playing, discarding\n", f->name, evt->base.on_pid ? evt->base.on_pid->name : "none", gf_filter_event_name(evt->base.type)));
		gf_free(evt);
		return;
	} else if (f->freg->process_event) {
		FSESS_CHECK_THREAD(f)
		canceled = f->freg->process_event(f, evt);
	}

	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s PID %s processed event %s - canceled %s\n", f->name, evt->base.on_pid ? evt->base.on_pid->name : "none", gf_filter_event_name(evt->base.type), canceled ? "yes" : "no" ));

	if (evt->base.on_pid && ((evt->base.type == GF_FEVT_STOP) || (evt->base.type==GF_FEVT_SOURCE_SEEK) || (evt->base.type==GF_FEVT_PLAY)) ) {
		u32 i;
		Bool do_reset = GF_TRUE;
		Bool is_play_reset = GF_FALSE;
		GF_FilterPidInst *p = (GF_FilterPidInst *) evt->base.on_pid;
		GF_FilterPid *pid = p->pid;
		//we need to force a PID reset when the first PLAY is > 0, since some filters may have dispatched packets during the initialization
		//phase
		if (evt->base.type==GF_FEVT_PLAY) {
			pid->is_playing = GF_TRUE;
			if (pid->initial_play_done) {
				do_reset = GF_FALSE;
			} else {
				pid->initial_play_done = GF_TRUE;
				is_play_reset = GF_TRUE;
				if (evt->play.start_range < 0.1)
					do_reset = GF_FALSE;
			}
		} else if (evt->base.type==GF_FEVT_STOP) {
			pid->is_playing = GF_FALSE;
		} else if (evt->base.type==GF_FEVT_SOURCE_SEEK) {
			pid->is_playing = GF_TRUE;
		}
		for (i=0; i<pid->num_destinations && do_reset; i++) {
			GF_FilterPidInst *pidi = gf_list_get(pid->destinations, i);
			pidi->discard_packets = GF_TRUE;
			if (is_play_reset)
				safe_int_inc(& pid->discard_input_packets );
				
			safe_int_inc(& pid->filter->stream_reset_pending );
			//post task on destination filter
			gf_fs_post_task(pidi->filter->session, gf_filter_pid_reset_task, pidi->filter, NULL, "reset_pid", pidi);
		}
		pid->nb_reaggregation_pending = 0;
	}
	//after  play or seek, request a process task for source filters or filters having pending packets
	if (!f->input_pids || f->pending_packets) {
		if ((evt->base.type==GF_FEVT_PLAY) || (evt->base.type==GF_FEVT_SOURCE_SEEK)) {
			gf_filter_post_process_task(f);
		}
	}
	if ((evt->base.type==GF_FEVT_PLAY) || (evt->base.type==GF_FEVT_SET_SPEED)) {
		if (evt->base.on_pid) {
			u32 scaler = (u32)  ( (evt->play.speed<0) ? -evt->play.speed : evt->play.speed ) * GF_FILTER_SPEED_SCALER;
			if (!scaler) scaler = GF_FILTER_SPEED_SCALER;
			if (scaler != evt->base.on_pid->playback_speed_scaler) {
				u32 prev_scaler = evt->base.on_pid->playback_speed_scaler;
				evt->base.on_pid->playback_speed_scaler = scaler;
				//lowering speed, we may need to trigger blocking
				if (scaler<prev_scaler)
					gf_filter_pid_would_block(evt->base.on_pid);
				//increasing speed, we may want to unblock
				else
					gf_filter_pid_check_unblock(evt->base.on_pid);
			}
		}
	}

	//no more input pids
	count = f->num_input_pids;
	if (count==0) canceled = GF_TRUE;

	if (canceled) {
		gf_free(evt);
		return;
	}
	if (!task->pid) dispatched_filters = gf_list_new();

	//otherwise forward event to each input PID
	for (i=0; i<count; i++) {
		GF_FilterEvent *an_evt;
		GF_FilterPidInst *pid_inst = gf_list_get(f->input_pids, i);
		GF_FilterPid *pid = pid_inst->pid;
		if (!pid) continue;

		if (dispatched_filters) {
			if (gf_list_find(dispatched_filters, pid_inst->pid->filter) >=0 )
				continue;

			gf_list_add(dispatched_filters, pid_inst->pid->filter);
		}

		//mark pid instance as about to be reset to avoid processing PID destroy task before
		if ((evt->base.type == GF_FEVT_STOP) || (evt->base.type==GF_FEVT_SOURCE_SEEK)) {
			pid_inst->discard_packets = GF_TRUE;
			safe_int_inc(& pid_inst->pid->discard_input_packets );
		}
		//allocate a copy except for the last PID where we use the one from the input
		if (i+1<count) {
			an_evt = gf_malloc(sizeof(GF_FilterEvent));
			memcpy(an_evt, evt, sizeof(GF_FilterEvent));
		} else {
			an_evt = evt;
		}
		an_evt->base.on_pid = task->pid ? pid : NULL;

		safe_int_inc(&pid->filter->num_events_queued);
		
		gf_fs_post_task(pid->filter->session, gf_filter_pid_send_event_downstream, pid->filter, task->pid ? pid : NULL, "downstream_event", an_evt);
	}
	if (dispatched_filters) gf_list_del(dispatched_filters);
	return;
}

void gf_filter_pid_send_event_upstream(GF_FSTask *task)
{
	u32 i, j;
	Bool canceled = GF_FALSE;
	GF_FilterEvent *evt = task->udta;
	GF_Filter *f = task->filter;

	if (f->stream_reset_pending) {
		TASK_REQUEUE(task)
		return;
	}

	assert(! evt->base.on_pid);

	canceled = f->freg->process_event ? f->freg->process_event(f, evt) : GF_TRUE;
	if (!canceled) {
		for (i=0; i<f->num_output_pids; i++) {
			GF_FilterPid *apid = gf_list_get(f->output_pids, i);
			for (j=0; j<apid->num_destinations; j++) {
				GF_FilterEvent *dup_evt;
				GF_FilterPidInst *pidi = gf_list_get(apid->destinations, j);

				dup_evt = gf_malloc(sizeof(GF_FilterEvent));
				memcpy(dup_evt, evt, sizeof(GF_FilterEvent));
				dup_evt->base.on_pid = NULL;
				gf_fs_post_task(pidi->filter->session, gf_filter_pid_send_event_upstream, pidi->filter, NULL, "upstream_event", dup_evt);
			}
		}
	}
	gf_free(evt);
}

void gf_filter_pid_send_event_internal(GF_FilterPid *pid, GF_FilterEvent *evt, Bool force_downstream)
{
	GF_FilterEvent *dup_evt;
	GF_FilterPid *target_pid=NULL;
	Bool upstream=GF_FALSE;
	if (!pid) {
		pid = evt->base.on_pid;
		if (!pid) return;
	}
	//filter is being shut down, prevent any event posting
	if (pid->filter->finalized) return;

	if (!force_downstream && PID_IS_OUTPUT(pid)) {
		upstream = GF_TRUE;
	}

	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s PID %s queuing %s event %s\n", pid->pid->filter->name, pid->pid->name, upstream ? "upstream" : "downstream", gf_filter_event_name(evt->base.type) ));

	if (upstream) {
		u32 i, j;
		for (i=0; i<pid->filter->num_output_pids; i++) {
			GF_FilterPid *apid = gf_list_get(pid->filter->output_pids, i);
			if (evt->base.on_pid && (apid != evt->base.on_pid)) continue;
			for (j=0; j<apid->num_destinations; j++) {
				GF_FilterPidInst *pidi = gf_list_get(apid->destinations, j);
				dup_evt = gf_malloc(sizeof(GF_FilterEvent));
				memcpy(dup_evt, evt, sizeof(GF_FilterEvent));
				dup_evt->base.on_pid = NULL;
				gf_fs_post_task(pidi->filter->session, gf_filter_pid_send_event_upstream, pidi->filter, NULL, "upstream_event", dup_evt);
			}
		}
		return;
	}


	if ((evt->base.type == GF_FEVT_STOP) || (evt->base.type == GF_FEVT_PLAY) || (evt->base.type==GF_FEVT_SOURCE_SEEK)) {
		u32 i, count = pid->pid->num_destinations;
		for (i=0; i<count; i++) {
			GF_FilterPidInst *pidi = gf_list_get(pid->pid->destinations, i);
			if (evt->base.type == GF_FEVT_PLAY) {
				pidi->is_end_of_stream = GF_FALSE;
			} else {
				//flag pid instance to discard all packets
				pidi->discard_packets = GF_TRUE;
				safe_int_inc(& pidi->pid->discard_input_packets );
			}
		}
	}

	dup_evt = gf_malloc(sizeof(GF_FilterEvent));
	memcpy(dup_evt, evt, sizeof(GF_FilterEvent));
	if (evt->base.on_pid) {
		target_pid = evt->base.on_pid->pid;
		dup_evt->base.on_pid = target_pid;
		safe_int_inc(&target_pid->filter->num_events_queued);
	}
	gf_fs_post_task(pid->pid->filter->session, gf_filter_pid_send_event_downstream, pid->pid->filter, target_pid, "downstream_event", dup_evt);
}

GF_EXPORT
void gf_filter_pid_send_event(GF_FilterPid *pid, GF_FilterEvent *evt)
{
	gf_filter_pid_send_event_internal(pid, evt, GF_FALSE);
}

GF_EXPORT
void gf_filter_send_event(GF_Filter *filter, GF_FilterEvent *evt)
{
	GF_FilterEvent *dup_evt;
	//filter is being shut down, prevent any event posting
	if (filter->finalized) return;

	if (evt->base.on_pid && PID_IS_OUTPUT(evt->base.on_pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Sending filter events upstream not yet implemented (PID %s in filter %s)\n", evt->base.on_pid->pid->name, filter->name));
		return;
	}

	dup_evt = gf_malloc(sizeof(GF_FilterEvent));
	memcpy(dup_evt, evt, sizeof(GF_FilterEvent));

	if (evt->base.on_pid) {
		safe_int_inc(&evt->base.on_pid->filter->num_events_queued);
	}

	gf_fs_post_task(filter->session, gf_filter_pid_send_event_downstream, filter, evt->base.on_pid, "downstream_event", dup_evt);
}


GF_EXPORT
void gf_filter_pid_exec_event(GF_FilterPid *pid, GF_FilterEvent *evt)
{
	//filter is being shut down, prevent any event posting
	if (pid->pid->filter->finalized) return;
	assert (pid->pid->filter->freg->flags &	GF_FS_REG_MAIN_THREAD);

	if (pid->pid->filter->freg->process_event) {
		if (evt->base.on_pid) evt->base.on_pid = evt->base.on_pid->pid;
		FSESS_CHECK_THREAD(pid->pid->filter)
		pid->pid->filter->freg->process_event(pid->pid->filter, evt);
	}
}


GF_EXPORT
Bool gf_filter_pid_is_filter_in_parents(GF_FilterPid *pid, GF_Filter *filter)
{
	if (!pid || !filter) return GF_FALSE;
	pid = pid->pid;
	return filter_in_parent_chain(pid->pid->filter, filter);
}


GF_EXPORT
GF_Err gf_filter_pid_get_statistics(GF_FilterPid *pid, GF_FilterPidStatistics *stats, Bool for_inputs)
{
	u32 i;
	GF_Filter *filter;
	GF_FilterPidInst *pidi = (GF_FilterPidInst *)pid;
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Getting statistics on output pids is not supported (PID %s in filter %s)\n", pid->pid->name, pid->filter->name));
		return GF_BAD_PARAM;
	}
	memset(stats, 0, sizeof(GF_FilterPidStatistics) );
	if (!for_inputs) {
		stats->avgerage_bitrate = pidi->avg_bit_rate;
		stats->first_process_time = pidi->first_frame_time;
		stats->last_process_time = pidi->last_pck_fetch_time;
		stats->max_bitrate = pidi->max_bit_rate;
		stats->max_process_time = (u32) pidi->max_process_time;
		stats->max_sap_process_time = (u32) pidi->max_sap_process_time;
		stats->min_frame_dur = pidi->pid->min_pck_duration;
		stats->nb_processed = pidi->nb_processed;
		stats->nb_saps = pidi->nb_sap_processed;
		stats->total_process_time = pidi->total_process_time;
		stats->total_sap_process_time = pidi->total_sap_process_time;

		stats->average_process_rate = pidi->avg_process_rate;
		stats->max_process_rate = pidi->max_process_rate;
		return GF_OK;
	}
	filter = pidi->pid->filter;
	for (i=0; i < filter->num_input_pids; i++) {
		pidi = (GF_FilterPidInst *) gf_list_get(filter->input_pids, i);

		stats->avgerage_bitrate += pidi->avg_bit_rate;
		if (!stats->first_process_time || (stats->first_process_time > pidi->first_frame_time))
			stats->first_process_time = pidi->first_frame_time;
		if (stats->last_process_time < pidi->last_pck_fetch_time)
			stats->last_process_time = pidi->last_pck_fetch_time;

		stats->max_bitrate += pidi->max_bit_rate;

		if (stats->max_process_time < (u32) pidi->max_process_time)
			stats->max_process_time = (u32) pidi->max_process_time;
		if (stats->max_sap_process_time < (u32) pidi->max_sap_process_time)
			stats->max_sap_process_time = (u32) pidi->max_sap_process_time;
		if (!stats->min_frame_dur || (stats->min_frame_dur > pidi->pid->min_pck_duration))
			stats->min_frame_dur = pidi->pid->min_pck_duration;
		stats->nb_processed += pidi->nb_processed;
		stats->nb_saps += pidi->nb_sap_processed;
		stats->total_process_time += pidi->total_process_time;
		stats->total_sap_process_time += pidi->total_sap_process_time;
		stats->average_process_rate += pidi->avg_process_rate;
		stats->max_process_rate += pidi->max_process_rate;
	}
	return GF_OK;
}

GF_EXPORT
void gf_filter_pid_remove(GF_FilterPid *pid)
{
	GF_FilterPacket *pck;
	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Removing PID input filter (%s:%s) not allowed\n", pid->filter->name, pid->pid->name));
	}
	GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Filter %s removed request output PID %s\n", pid->filter->name, pid->pid->name));

	if (pid->filter->removed) {
		return;
	}
	if (pid->removed) {
		return;
	}
	pid->removed = GF_TRUE;

	//we create a fake packet for eos signaling
	pck = gf_filter_pck_new_shared_internal(pid, NULL, 0, NULL, GF_TRUE);
	gf_filter_pck_set_framing(pck, GF_TRUE, GF_TRUE);
	pck->pck->info.flags |= GF_PCK_CMD_PID_REM;
	gf_filter_pck_send(pck);
}

GF_EXPORT
void gf_filter_pid_try_pull(GF_FilterPid *pid)
{
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to pull from output PID %s in filter %s\n", pid->pid->name, pid->filter->name));
		return;
	}
	pid = pid->pid;
	if (pid->filter->session->threads) {
		GF_LOG(GF_LOG_DEBUG, GF_LOG_FILTER, ("Filter pull in multithread mode not yet implementing - defaulting to 1 ms sleep\n", pid->pid->name, pid->filter->name));
		gf_sleep(1);
		return;
	}

	gf_filter_process_inline(pid->filter);
}


GF_EXPORT
GF_FilterClockType gf_filter_pid_get_clock_info(GF_FilterPid *pid, u64 *clock_time, u32 *timescale)
{
	GF_FilterPidInst *pidi = (GF_FilterPidInst *)pid;
	GF_FilterClockType res;
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Querying clock on output PID %s in filter %s\n", pid->pid->name, pid->filter->name));
		return GF_FILTER_CLOCK_NONE;
	}
	if (clock_time) *clock_time = pidi->last_clock_value;
	if (timescale) *timescale = pidi->last_clock_timescale;
	res = pidi->last_clock_type;
	pidi->last_clock_type = 0;
	return res;
}

GF_EXPORT
u32 gf_filter_pid_get_timescale(GF_FilterPid *pid)
{
	GF_PropertyMap *map = pid ? gf_list_get(pid->pid->properties, 0) : 0;
	return map ? map->timescale : 0;
}

GF_EXPORT
void gf_filter_pid_clear_eos(GF_FilterPid *pid)
{
	GF_FilterPidInst *pidi = (GF_FilterPidInst *)pid;
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Clearing EOS on output PID %s in filter %s\n", pid->pid->name, pid->filter->name));
		return;
	}
	pidi->is_end_of_stream = GF_FALSE;
}

GF_EXPORT
void gf_filter_pid_set_clock_mode(GF_FilterPid *pid, Bool filter_in_charge)
{
	GF_FilterPidInst *pidi = (GF_FilterPidInst *)pid;
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Changing clock mode on output PID %s in filter %s\n", pid->pid->name, pid->filter->name));
		return;
	}
	pidi->handles_clock_references = filter_in_charge;
}

GF_EXPORT
const char *gf_filter_pid_get_args(GF_FilterPid *pid)
{
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Querying args on output PID %s in filter %s\n", pid->pid->name, pid->filter->name));
		return NULL;
	}
	if (pid->pid->filter->src_args) return pid->pid->filter->src_args;
	return pid->pid->filter->orig_args;
}

GF_EXPORT
void gf_filter_pid_set_max_buffer(GF_FilterPid *pid, u32 total_duration_us)
{
	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Setting max buffer on input PID %s in filter %s not allowed\n", pid->pid->name, pid->filter->name));
		return;
	}
	pid->max_buffer_time = pid->user_max_buffer_time = total_duration_us;
}

GF_EXPORT
u32 gf_filter_pid_get_max_buffer(GF_FilterPid *pid)
{
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Querying max buffer on output PID %s in filter %s not allowed\n", pid->pid->name, pid->filter->name));
		return 0;
	}
	return pid->pid->user_max_buffer_time;
}


GF_EXPORT
void gf_filter_pid_set_loose_connect(GF_FilterPid *pid)
{
	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Setting loose connect on input PID %s in filter %s not allowed\n", pid->pid->name, pid->filter->name));
		return;
	}
	pid->not_connected_ok = GF_TRUE;
}

GF_EXPORT
const GF_PropertyValue *gf_filter_pid_caps_query(GF_FilterPid *pid, u32 prop_4cc)
{
	u32 i;
	GF_PropertyMap *map = pid->caps_negociate;
	if (PID_IS_INPUT(pid)) {
		u32 k;
		GF_Filter *dst;
		if (!pid->filter->dst_filter || (pid->filter->dst_filter->cap_idx_at_resolution<0) ) {
			GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Reconfig caps query on input PID %s in filter %s with no destination filter set\n", pid->pid->name, pid->filter->name));
			return NULL;
		}
		dst = pid->filter->dst_filter;
		for (k=dst->cap_idx_at_resolution; k<dst->freg->nb_caps; k++) {
			const GF_FilterCapability *cap = &dst->freg->caps[k];
			if (!(cap->flags & GF_CAPFLAG_IN_BUNDLE)) return NULL;

			if (!(cap->flags & GF_CAPFLAG_INPUT)) continue;
			if (cap->flags & GF_CAPFLAG_OPTIONAL) continue;
			if (cap->code == prop_4cc) return &cap->val;
		}
		return NULL;
	}
	if (map) return gf_props_get_property(map, prop_4cc, NULL);
	for (i=0; i<pid->num_destinations; i++) {
		u32 j;
		GF_FilterPidInst *pidi = gf_list_get(pid->destinations, i);
		for (j=0; j<pidi->filter->nb_forced_caps; j++) {
			if (pidi->filter->forced_caps[j].code==prop_4cc)
				return &pidi->filter->forced_caps[j].val;
		}
		//walk up the chain
		for (j=0; j<pidi->filter->num_output_pids; j++) {
			GF_FilterPid *apid = gf_list_get(pidi->filter->output_pids, j);
			if (apid) {
				const GF_PropertyValue *p = gf_filter_pid_caps_query(apid, prop_4cc);
				if (p) return p;
			}
		}

	}

	//trick here: we may not be connected yet (called during a configure_pid), use the target destination
	//of the filter as caps source
	if (pid->filter->dst_filter) {
		GF_Filter *a_filter = pid->filter->dst_filter;
		while (a_filter) {
			for (i=0; i<a_filter->nb_forced_caps; i++) {
				if (a_filter->forced_caps[i].code==prop_4cc)
					return &a_filter->forced_caps[i].val;
			}
			a_filter = a_filter->dst_filter;
		}
	}

	return NULL;
}

GF_EXPORT
const GF_PropertyValue *gf_filter_pid_caps_query_str(GF_FilterPid *pid, const char *prop_name)
{
	GF_PropertyMap *map = pid->caps_negociate;
	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Reconfig caps query on input PID %s in filter %s not allowed\n", pid->pid->name, pid->filter->name));
		return NULL;
	}
	return map ? gf_props_get_property(map, 0, prop_name) : NULL;
}


GF_EXPORT
GF_Err gf_filter_pid_resolve_file_template(GF_FilterPid *pid, char szTemplate[GF_MAX_PATH], char szFinalName[GF_MAX_PATH], u32 file_idx, const char *file_suffix)
{
	u32 k;
	char szFormat[30], szTemplateVal[GF_MAX_PATH], szPropVal[GF_PROP_DUMP_ARG_SIZE];
	char *name = szTemplate;
	if (!strchr(szTemplate, '$')) {
		strcpy(szFinalName, szTemplate);
		return GF_OK;
	}
	
	k = 0;
	while (name[0]) {
		char *sep=NULL;
		char *fsep=NULL;
		const char *str_val = NULL;
		s64 value = 0;
		Bool is_ok = GF_TRUE;
		Bool do_skip = GF_FALSE;
		Bool has_val = GF_FALSE;
		Bool is_file_str = GF_FALSE;
		u32 prop_4cc = 0;
		GF_PropertyValue prop_val_patched;
		const GF_PropertyValue *prop_val = NULL;

		if (k+1==GF_MAX_PATH) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("[Filter] Not enough memory to solve file template %s\n", szTemplate));
			return GF_OUT_OF_MEM;
		}
		if (name[0] != '$') {
			szFinalName[k] = name[0];
			k++;
			name++;
			continue;
		}
		if (name[1]=='$') {
			szFinalName[k] = '$';
			name++;
			k++;
			continue;
		}
		sep = strchr(name+1, '$');
		if (!sep) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[Filter] broken file template expecting $KEYWORD$, couln't find second '$'\n", szTemplate));
			strcpy(szFinalName, szTemplate);
			return GF_BAD_PARAM;
		}
		szFormat[0] = '%';
		szFormat[1] = 0;

		szFinalName[k] = 0;
		name++;
		sep[0]=0;
		fsep = strchr(name, '%');
		if (fsep) {
			u32 len;
			name = fsep+1;
			while (name[0] && (strchr("0123456789.", name[0]))) {
				name++;
			}
			len = (u32) (name - fsep);
			strncpy(szFormat, fsep, len);
			szFormat[len]=0;
		}
		strcat(szFormat, "d");

		if (!strcmp(name, "num")) {
			name += 3;
			value = file_idx;
			has_val = GF_TRUE;
		} else if (!strcmp(name, "URL")) {
			prop_val = gf_filter_pid_get_property(pid, GF_PROP_PID_URL);
			is_file_str = GF_TRUE;
		} else if (!strcmp(name, "File")) {
			prop_val = gf_filter_pid_get_property(pid, GF_PROP_PID_FILEPATH);
			if (!prop_val) prop_val = gf_filter_pid_get_property(pid, GF_PROP_PID_URL);
			is_file_str = GF_TRUE;
		} else if (!strcmp(name, "PID")) {
			prop_val = gf_filter_pid_get_property(pid, GF_PROP_PID_ID);
		} else if (!strcmp(name, "DS")) {
			str_val = file_suffix ? file_suffix : "";
			is_ok = GF_TRUE;
		} else if (!strncmp(name, "p4cc=", 5)) {
			if (strlen(name) != 9) {
				GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[Filter] wrong length in 4CC template, expecting 4cc=ABCD\n", name));
				is_ok = GF_FALSE;
			} else {
				prop_4cc = GF_4CC(name[5],name[6],name[7],name[8]);
				prop_val = gf_filter_pid_get_property(pid, prop_4cc);
				if (!prop_val) {
					GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[Filter] no pid property of type %s\n", name+5));
					is_ok = GF_FALSE;
				}
			}
		} else if (!strncmp(name, "pname=", 6)) {
			prop_val = gf_filter_pid_get_property_str(pid, name+6);
			if (!prop_val) {
				GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[Filter] no pid property named %s\n", name+6));
				is_ok = GF_FALSE;
			}
		} else if (!strncmp(name, "Number", 6)) {
			do_skip = GF_TRUE;
		} else if (!strncmp(name, "Time", 4)) {
			do_skip = GF_TRUE;
		} else if (!strncmp(name, "RepresentationID", 16)) {
			do_skip = GF_TRUE;
		} else if (!strncmp(name, "Bandwidth", 9)) {
			do_skip = GF_TRUE;
		} else if (!strncmp(name, "SubNumber", 9)) {
			do_skip = GF_TRUE;
		} else {
			char *next_eq = strchr(name, '=');
			char *next_sep = strchr(name, '$');
			if (!next_eq || (next_eq - name < next_sep - name)) {
				prop_4cc = gf_props_get_id(name);
				//not matching, try with name
				if (!prop_4cc) {
					prop_val = gf_filter_pid_get_property_str(pid, name);
					if (!prop_val) {
						GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[Filter] Unrecognized template %s\n", name));
						is_ok = GF_FALSE;
					}
				} else {
					prop_val = gf_filter_pid_get_property(pid, prop_4cc);
					if (!prop_val) {
						is_ok = GF_FALSE;
					}
				}
			} else {
				u32 i, len = (u32) (next_sep ? 1+(next_sep - name) : strlen(name) );
				szFinalName[k]='$';
				k++;
				for (i=0; i<len; i++) {
					szFinalName[k] = name[0];
					k++;
					name++;
				}
				szFinalName[k]='$';
				k++;
				if (!sep) break;
				sep[0] = '$';
				name = sep+1;
				continue;
			}
		}
		if (do_skip) {
			if (sep) sep[0] = '$';
			szFinalName[k] = '$';
			k++;
			while (name[0] && (name[0] != '$')) {
				szFinalName[k] = name[0];
				k++;
				name++;
			}
			szFinalName[k] = '$';
			k++;
			name++;


			continue;

		}


		if (!is_ok && !prop_val && prop_4cc) {
			if (prop_4cc==GF_PROP_PID_CROP_POS) {
				prop_val_patched.type = GF_PROP_VEC2I;
				prop_val_patched.value.vec2i.x = 0;
				prop_val_patched.value.vec2i.y = 0;
				prop_val = &prop_val_patched;
				is_ok=GF_TRUE;
			}
			else if (prop_4cc==GF_PROP_PID_ORIG_SIZE) {
				prop_val_patched.type = GF_PROP_VEC2I;
				prop_val = gf_filter_pid_get_property(pid, GF_PROP_PID_WIDTH);
				prop_val_patched.value.vec2i.x = prop_val ? prop_val->value.uint : 0;
				prop_val = gf_filter_pid_get_property(pid, GF_PROP_PID_HEIGHT);
				prop_val_patched.value.vec2i.y = prop_val ? prop_val->value.uint : 0;
				prop_val = &prop_val_patched;
				is_ok=GF_TRUE;
			} else {
				GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[Filter] property %s not found for pid, cannot resolve template\n", name));
				return GF_BAD_PARAM;
			}
		}

		if (!is_ok) {
			if (sep) sep[0] = '$';
			return GF_BAD_PARAM;
		}
		if (prop_val) {
			if ((prop_val->type==GF_PROP_UINT) || (prop_val->type==GF_PROP_SINT)) {
				value = prop_val->value.uint;
				has_val = GF_TRUE;
			} else {
				str_val = gf_prop_dump_val(prop_val, szPropVal, GF_FALSE, NULL);
			}
		}
		szTemplateVal[0]=0;
		if (has_val) {
			sprintf(szTemplateVal, szFormat, value);
			str_val = szTemplateVal;
		} else if (str_val) {
			if (is_file_str) {
				char *ext;
				char *sname = strrchr(str_val, '/');
				if (!sname) sname = strrchr(str_val, '\\');
				if (!sname) sname = (char *) str_val;
				else sname++;
				ext = strrchr(str_val, '.');

				if (ext) {
					u32 len = (u32) (ext - sname);
					strncpy(szTemplateVal, sname, ext - sname);
					szTemplateVal[len] = 0;
				} else {
					strcpy(szTemplateVal, sname);
				}
			} else {
				strcpy(szTemplateVal, str_val);
			}
		} else {
			GF_LOG(GF_LOG_WARNING, GF_LOG_MMIO, ("[Filter] property %s not found for pid, cannot resolve template\n", name));
			return GF_BAD_PARAM;
		}
		if (k + strlen(szTemplateVal) > GF_MAX_PATH) {
			GF_LOG(GF_LOG_WARNING, GF_LOG_FILTER, ("[Filter] Not enough memory to solve file template %s\n", szTemplate));
			return GF_OUT_OF_MEM;
		}

		strcat(szFinalName, szTemplateVal);
		k = (u32) strlen(szFinalName);

		if (!sep) break;
		sep[0] = '$';
		name = sep+1;
	}
	szFinalName[k] = 0;
	return GF_OK;
}


GF_EXPORT
GF_Err gf_filter_pid_set_discard(GF_FilterPid *pid, Bool discard_on)
{
	GF_FilterPidInst *pidi = (GF_FilterPidInst *) pid;
	if (PID_IS_OUTPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt at discarding packets on output PID %s in filter %s not allowed\n", pid->pid->name, pid->filter->name));
		return GF_BAD_PARAM;
	}
	if (discard_on) {
		GF_LOG(GF_LOG_INFO, GF_LOG_FILTER, ("Discarding packets on PID %s in filter %s\n", pid->pid->name, pid->filter->name));
		while (gf_filter_pid_get_packet(pid)) {
			gf_filter_pid_drop_packet(pid);
		}
		pidi->is_end_of_stream = GF_TRUE;
	} else {
		pidi->is_end_of_stream = pid->has_seen_eos;
	}
	pidi->discard_inputs = discard_on;
	return GF_OK;
}

static char *gf_filter_pid_get_dst_string(GF_FilterSession *sess, const char *dst_args)
{
	char *dst, *sep;
	char szKey[6];
	if (!dst_args) return NULL;

	sprintf(szKey, "dst%c", sess->sep_name);
	dst = strstr(dst_args, szKey);
	if (!dst) return NULL;

	sep = strchr(dst, sess->sep_args);
	if (sep && (sess->sep_args==':') && !strncmp(sep, "://", 3)) {
		char *sep2 = strchr(sep+3, '/');
		if (sep2) sep = strchr(sep+1, ':');
		else sep = strchr(sep+3, ':');
	}
	if (dst && sep) {
		u32 len;
		dst += 4;
		len = (u32) (sep - dst);
		char *res = gf_malloc(sizeof(char)* (len+1));
		memcpy(res, dst, sizeof(char)* len);
		res[len]=0;
		return res;
	}
	return NULL;
}

GF_EXPORT
char *gf_filter_pid_get_destination(GF_FilterPid *pid)
{
	const char *dst_args;
	char *res;
	u32 i, j;
	if (PID_IS_INPUT(pid)) {
		GF_LOG(GF_LOG_ERROR, GF_LOG_FILTER, ("Attempt to query destination on input PID %s in filter %s not allowed\n", pid->pid->name, pid->filter->name));
		return NULL;
	}

	dst_args = pid->filter->dst_args;
	if (!dst_args) dst_args = pid->filter->src_args;
	res = gf_filter_pid_get_dst_string(pid->filter->session, dst_args);
	if (res) return res;

	//if not set this means we have explicetly loaded the filter
	for (i=0; i<pid->num_destinations; i++) {
		GF_FilterPidInst *pidi = gf_list_get(pid->destinations, i);

		dst_args = pidi->filter->dst_args;
		if (!dst_args) dst_args = pidi->filter->src_args;
		res = gf_filter_pid_get_dst_string(pid->filter->session, dst_args);
		if (res) return res;

		for (j=0; j<pidi->filter->num_output_pids; j++) {
			GF_FilterPid *a_pid = gf_list_get(pidi->filter->output_pids, j);
			char *dst = gf_filter_pid_get_destination(a_pid);
			if (dst) return dst;
		}
	}
	return NULL;
}

