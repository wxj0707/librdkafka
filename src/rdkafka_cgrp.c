/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2015, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rdkafka_int.h"
#include "rdkafka_broker.h"
#include "rdkafka_request.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_assignor.h"
#include "rdkafka_offset.h"
#include "rdkafka_metadata.h"
#include "rdkafka_cgrp.h"

static void rd_kafka_cgrp_check_unassign_done (rd_kafka_cgrp_t *rkcg,
                                               const char *reason);
static void rd_kafka_cgrp_offset_commit_tmr_cb (rd_kafka_timers_t *rkts,
                                                void *arg);
static void rd_kafka_cgrp_assign (rd_kafka_cgrp_t *rkcg,
				  rd_kafka_topic_partition_list_t *assignment);
static rd_kafka_resp_err_t rd_kafka_cgrp_unassign (rd_kafka_cgrp_t *rkcg);
static void
rd_kafka_cgrp_partitions_fetch_start0 (rd_kafka_cgrp_t *rkcg,
				       rd_kafka_topic_partition_list_t
				       *assignment, int usable_offsets,
				       int line);
#define rd_kafka_cgrp_partitions_fetch_start(rkcg,assignment,usable_offsets) \
	rd_kafka_cgrp_partitions_fetch_start0(rkcg,assignment,usable_offsets,\
					      __LINE__)

/**
 * @returns true if cgrp can start partition fetchers, which is true if
 *          there is a subscription and the group is fully joined, or there
 *          is no subscription (in which case the join state is irrelevant) - 
 *          such as for an assign() without subscribe(). */
#define RD_KAFKA_CGRP_CAN_FETCH_START(rkcg) \
	((rkcg)->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_ASSIGNED)

/**
 * @returns true if cgrp is waiting for a rebalance_cb to be handled by
 *          the application.
 */
#define RD_KAFKA_CGRP_WAIT_REBALANCE_CB(rkcg)			\
	((rkcg)->rkcg_join_state ==				\
	 RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_REBALANCE_CB ||	\
	 (rkcg)->rkcg_join_state ==				\
	 RD_KAFKA_CGRP_JOIN_STATE_WAIT_REVOKE_REBALANCE_CB)


const char *rd_kafka_cgrp_state_names[] = {
        "init",
        "term",
        "query-coord",
        "wait-coord",
        "wait-broker",
        "wait-broker-transport",
        "up"
};

const char *rd_kafka_cgrp_join_state_names[] = {
        "init",
        "wait-join",
        "wait-metadata",
        "wait-sync",
        "wait-unassign",
        "wait-assign-rebalance_cb",
	"wait-revoke-rebalance_cb",
        "assigned",
	"started"
};


static void rd_kafka_cgrp_set_state (rd_kafka_cgrp_t *rkcg, int state) {
        if ((int)rkcg->rkcg_state == state)
                return;

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPSTATE",
                     "Group \"%.*s\" changed state %s -> %s "
                     "(v%d, join-state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                     rd_kafka_cgrp_state_names[state],
		     rkcg->rkcg_version,
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);
        rkcg->rkcg_state = state;
        rkcg->rkcg_ts_statechange = rd_clock();

	rd_kafka_brokers_broadcast_state_change(rkcg->rkcg_rk);
}


void rd_kafka_cgrp_set_join_state (rd_kafka_cgrp_t *rkcg, int join_state) {
        if ((int)rkcg->rkcg_join_state == join_state)
                return;

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPJOINSTATE",
                     "Group \"%.*s\" changed join state %s -> %s "
                     "(v%d, state %s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                     rd_kafka_cgrp_join_state_names[join_state],
		     rkcg->rkcg_version,
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state]);
        rkcg->rkcg_join_state = join_state;
}


static RD_INLINE void
rd_kafka_cgrp_version_new_barrier0 (rd_kafka_cgrp_t *rkcg,
				    const char *func, int line) {
	rkcg->rkcg_version++;
	rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "BARRIER",
		     "Group \"%.*s\": %s:%d: new version barrier v%d",
		     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), func, line,
		     rkcg->rkcg_version);
}

#define rd_kafka_cgrp_version_new_barrier(rkcg) \
	rd_kafka_cgrp_version_new_barrier0(rkcg, __FUNCTION__, __LINE__)


void rd_kafka_cgrp_destroy_final (rd_kafka_cgrp_t *rkcg) {
        rd_kafka_assert(rkcg->rkcg_rk, !rkcg->rkcg_assignment);
        rd_kafka_assert(rkcg->rkcg_rk, !rkcg->rkcg_subscription);
        rd_kafka_assert(rkcg->rkcg_rk, !rkcg->rkcg_group_leader.members);
        rd_kafka_cgrp_set_member_id(rkcg, NULL);

        rd_kafka_q_destroy(rkcg->rkcg_q);
        rd_kafka_q_destroy(rkcg->rkcg_ops);
	rd_kafka_q_destroy(rkcg->rkcg_wait_coord_q);
        rd_kafka_assert(rkcg->rkcg_rk, TAILQ_EMPTY(&rkcg->rkcg_topics));
        rd_kafka_assert(rkcg->rkcg_rk, rd_list_empty(&rkcg->rkcg_toppars));
        rd_list_destroy(&rkcg->rkcg_toppars, NULL);
	rd_list_destroy(rkcg->rkcg_subscribed_topics,
			(void *)rd_kafka_topic_info_destroy);
        rd_free(rkcg);
}




rd_kafka_cgrp_t *rd_kafka_cgrp_new (rd_kafka_t *rk,
                                    const rd_kafkap_str_t *group_id,
                                    const rd_kafkap_str_t *client_id) {
        rd_kafka_cgrp_t *rkcg;

        rkcg = rd_calloc(1, sizeof(*rkcg));

        rkcg->rkcg_rk = rk;
        rkcg->rkcg_group_id = group_id;
        rkcg->rkcg_client_id = client_id;
        rkcg->rkcg_coord_id = -1;
        rkcg->rkcg_generation_id = -1;
	rkcg->rkcg_version = 1;

        mtx_init(&rkcg->rkcg_lock, mtx_plain);
        rkcg->rkcg_ops = rd_kafka_q_new(rk);
        rkcg->rkcg_q = rd_kafka_q_new(rk);
	rkcg->rkcg_wait_coord_q = rd_kafka_q_new(rk);
        TAILQ_INIT(&rkcg->rkcg_topics);
        rd_list_init(&rkcg->rkcg_toppars, 32);
        rd_kafka_cgrp_set_member_id(rkcg, "");
	rkcg->rkcg_subscribed_topics = rd_list_new(0);
        rd_interval_init(&rkcg->rkcg_coord_query_intvl);
        rd_interval_init(&rkcg->rkcg_heartbeat_intvl);
        rd_interval_init(&rkcg->rkcg_join_intvl);
        rd_interval_init(&rkcg->rkcg_timeout_scan_intvl);

        if (RD_KAFKAP_STR_IS_NULL(group_id)) {
                /* No group configured: Operate in legacy/SimpleConsumer mode */
                rd_kafka_simple_consumer_add(rk);
                /* no need look up group coordinator (no queries) */
                rd_interval_disable(&rkcg->rkcg_coord_query_intvl);
        }

        if (rk->rk_conf.enable_auto_commit &&
            rk->rk_conf.auto_commit_interval_ms > 0)
                rd_kafka_timer_start(&rk->rk_timers,
                                     &rkcg->rkcg_offset_commit_tmr,
                                     rk->rk_conf.
				     auto_commit_interval_ms * 1000ll,
                                     rd_kafka_cgrp_offset_commit_tmr_cb,
                                     rkcg);

        return rkcg;
}



/**
 * Select a broker to handle this cgrp.
 * It will prefer the coordinator broker but if that is not available
 * any other broker that is Up will be used, and if that also fails
 * uses the internal broker handle.
 *
 * NOTE: The returned rkb will have had its refcnt increased.
 */
static rd_kafka_broker_t *rd_kafka_cgrp_select_broker (rd_kafka_cgrp_t *rkcg) {
        rd_kafka_broker_t *rkb = NULL;


        /* No need for a managing broker when cgrp is terminated */
        if (rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_TERM)
                return NULL;

        rd_kafka_rdlock(rkcg->rkcg_rk);
        /* Try to find the coordinator broker, if it isn't found
         * move the cgrp to any other Up broker which will
         * do further coord querying while waiting for the
         * proper broker to materialise.
         * If that also fails, go with the internal broker */
        if (rkcg->rkcg_coord_id != -1)
                rkb = rd_kafka_broker_find_by_nodeid(rkcg->rkcg_rk,
                                                     rkcg->rkcg_coord_id);
        if (!rkb)
                rkb = rd_kafka_broker_prefer(rkcg->rkcg_rk,
                                             rkcg->rkcg_coord_id,
                                             RD_KAFKA_BROKER_STATE_UP);
        if (!rkb)
                rkb = rd_kafka_broker_internal(rkcg->rkcg_rk);

        rd_kafka_rdunlock(rkcg->rkcg_rk);

        /* Dont change managing broker unless warranted.
         * This means do not change to another non-coordinator broker
         * while we are waiting for the proper coordinator broker to
         * become available. */
        if (rkb && rkcg->rkcg_rkb && rkb != rkcg->rkcg_rkb) {
		int old_is_coord, new_is_coord;

		rd_kafka_broker_lock(rkb);
		new_is_coord = RD_KAFKA_CGRP_BROKER_IS_COORD(rkcg, rkb);
		rd_kafka_broker_unlock(rkb);

		rd_kafka_broker_lock(rkcg->rkcg_rkb);
		old_is_coord = RD_KAFKA_CGRP_BROKER_IS_COORD(rkcg,
							     rkcg->rkcg_rkb);
		rd_kafka_broker_unlock(rkcg->rkcg_rkb);

		if (!old_is_coord && !new_is_coord &&
		    rkcg->rkcg_rkb->rkb_source != RD_KAFKA_INTERNAL) {
			rd_kafka_broker_destroy(rkb);
			rkb = rkcg->rkcg_rkb;
			rd_kafka_broker_keep(rkb);
		}
        }

        return rkb;
}




/**
 * Assign cgrp to broker.
 *
 * Locality: rdkafka main thread
 */
static void rd_kafka_cgrp_assign_broker (rd_kafka_cgrp_t *rkcg,
					 rd_kafka_broker_t *rkb) {

	rd_kafka_assert(NULL, rkcg->rkcg_rkb == NULL);

	rkcg->rkcg_rkb = rkb;
	rd_kafka_broker_keep(rkb);

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "BRKASSIGN",
                     "Group \"%.*s\" management assigned to broker %s",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_broker_name(rkb));

        /* Reset query interval to trigger an immediate
         * coord query if required */
        if (!rd_interval_disabled(&rkcg->rkcg_coord_query_intvl))
                rd_interval_reset(&rkcg->rkcg_coord_query_intvl);

        if (RD_KAFKA_CGRP_BROKER_IS_COORD(rkcg, rkb))
                rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_WAIT_BROKER_TRANSPORT);

}


/**
 * Unassign cgrp from current broker.
 *
 * Locality: main thread
 */
static void rd_kafka_cgrp_unassign_broker (rd_kafka_cgrp_t *rkcg) {
        rd_kafka_broker_t *rkb = rkcg->rkcg_rkb;

	rd_kafka_assert(NULL, rkcg->rkcg_rkb);
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "BRKUNASSIGN",
                     "Group \"%.*s\" management unassigned "
                     "from broker handle %s",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_broker_name(rkb));

        rkcg->rkcg_rkb = NULL;
        rd_kafka_broker_destroy(rkb); /* from assign() */
}


/**
 * Assign cgrp to a broker to handle.
 * It will prefer the coordinator broker but if that is not available
 * any other broker that is Up will be used, and if that also fails
 * uses the internal broker handle.
 *
 * Returns 1 if the cgrp was reassigned, else 0.
 */
int rd_kafka_cgrp_reassign_broker (rd_kafka_cgrp_t *rkcg) {
        rd_kafka_broker_t *rkb;

        rkb = rd_kafka_cgrp_select_broker(rkcg);

        if (rkb == rkcg->rkcg_rkb) {
		int is_coord = 0;

		if (rkb) {
			rd_kafka_broker_lock(rkb);
			is_coord = RD_KAFKA_CGRP_BROKER_IS_COORD(rkcg, rkb);
			rd_kafka_broker_unlock(rkb);
		}
		if (is_coord)
                        rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_WAIT_BROKER_TRANSPORT);
                else
                        rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_WAIT_BROKER);

                if (rkb)
                        rd_kafka_broker_destroy(rkb);
                return 0; /* No change */
        }

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "BRKREASSIGN",
                     "Group \"%.*s\" management reassigned from "
                     "broker %s to %s",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rkcg->rkcg_rkb ?
                     rd_kafka_broker_name(rkcg->rkcg_rkb) : "(none)",
                     rkb ? rd_kafka_broker_name(rkb) : "(none)");


        if (rkcg->rkcg_rkb)
                rd_kafka_cgrp_unassign_broker(rkcg);

        rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_WAIT_BROKER);

        if (rkb) {
		rd_kafka_cgrp_assign_broker(rkcg, rkb);
		rd_kafka_broker_destroy(rkb); /* from select_broker() */
	}

        return 1;
}


/**
 * Update the cgrp's coordinator and move it to that broker.
 */
void rd_kafka_cgrp_coord_update (rd_kafka_cgrp_t *rkcg, int32_t coord_id) {

        if (rkcg->rkcg_coord_id == coord_id) {
		if (rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_WAIT_COORD)
			rd_kafka_cgrp_set_state(rkcg,
						RD_KAFKA_CGRP_STATE_WAIT_BROKER);
                return;
	}

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPCOORD",
                     "Group \"%.*s\" changing coordinator %"PRId32" -> %"PRId32,
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), rkcg->rkcg_coord_id,
                     coord_id);
        rkcg->rkcg_coord_id = coord_id;

        rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_WAIT_BROKER);

        rd_kafka_cgrp_reassign_broker(rkcg);
}






/**
 * Handle GroupCoordinator response
 */
static void rd_kafka_cgrp_handle_GroupCoordinator (rd_kafka_t *rk,
						   rd_kafka_broker_t *rkb,
                                                   rd_kafka_resp_err_t err,
                                                   rd_kafka_buf_t *rkbuf,
                                                   rd_kafka_buf_t *request,
                                                   void *opaque) {
        const int log_decode_errors = 1;
        int16_t ErrorCode = 0;
        int32_t CoordId;
        rd_kafkap_str_t CoordHost;
        int32_t CoordPort;
        rd_kafka_cgrp_t *rkcg = opaque;
        struct rd_kafka_metadata_broker mdb = RD_ZERO_INIT;

        if (likely(!(ErrorCode = err))) {
                rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
                rd_kafka_buf_read_i32(rkbuf, &CoordId);
                rd_kafka_buf_read_str(rkbuf, &CoordHost);
                rd_kafka_buf_read_i32(rkbuf, &CoordPort);
        }

        if (ErrorCode)
                goto err2;


        mdb.id = CoordId;
	RD_KAFKAP_STR_DUPA(&mdb.host, &CoordHost);
	mdb.port = CoordPort;

        rd_rkb_dbg(rkb, CGRP, "CGRPCOORD",
                   "Group \"%.*s\" coordinator is %s:%i id %"PRId32,
                   RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                   mdb.host, mdb.port, mdb.id);
        rd_kafka_broker_update(rkb->rkb_rk, rkb->rkb_proto, &mdb);

        rd_kafka_cgrp_coord_update(rkcg, CoordId);
        return;

err: /* Parse error */
        ErrorCode = RD_KAFKA_RESP_ERR__BAD_MSG;
        /* FALLTHRU */

err2:
        rd_rkb_dbg(rkb, CGRP, "CGRPCOORD",
                   "Group \"%.*s\" GroupCoordinator response error: %s",
                   RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                   rd_kafka_err2str(ErrorCode));

        if (ErrorCode == RD_KAFKA_RESP_ERR_GROUP_COORDINATOR_NOT_AVAILABLE)
                rd_kafka_cgrp_coord_update(rkcg, -1);
	else if (rkcg->rkcg_last_err != ErrorCode) {
		rd_kafka_q_op_err(rkcg->rkcg_q, RD_KAFKA_OP_CONSUMER_ERR,
				  ErrorCode, 0, NULL, 0,
				  "GroupCoordinator response error: %s",
				  rd_kafka_err2str(ErrorCode));

		/* Suppress repeated errors */
                rkcg->rkcg_last_err = ErrorCode;

		/* Continue querying */
		rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_QUERY_COORD);
        }

}


/**
 * Query for coordinator.
 * Ask any broker in state UP
 *
 * Locality: main thread
 */
void rd_kafka_cgrp_coord_query (rd_kafka_cgrp_t *rkcg,
				const char *reason) {
	rd_kafka_broker_t *rkb;

	rd_kafka_rdlock(rkcg->rkcg_rk);
	rkb = rd_kafka_broker_any(rkcg->rkcg_rk, RD_KAFKA_BROKER_STATE_UP,
				  rd_kafka_broker_filter_can_group_query, NULL);
	rd_kafka_rdunlock(rkcg->rkcg_rk);

	if (!rkb) {
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPQUERY",
			     "Group \"%.*s\": "
			     "no broker available for coordinator query: %s",
			     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), reason);
		return;
	}

        rd_rkb_dbg(rkb, CGRP, "CGRPQUERY",
                   "Group \"%.*s\": querying for coordinator: %s",
                   RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), reason);

        rd_kafka_GroupCoordinatorRequest(rkb, rkcg->rkcg_group_id,
                                         RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
                                         rd_kafka_cgrp_handle_GroupCoordinator,
                                         rkcg);

        if (rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_QUERY_COORD)
                rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_WAIT_COORD);

	rd_kafka_broker_destroy(rkb);
}



static void rd_kafka_cgrp_leave (rd_kafka_cgrp_t *rkcg, int ignore_response) {
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "LEAVE",
                   "Group \"%.*s\": leave",
                   RD_KAFKAP_STR_PR(rkcg->rkcg_group_id));

        if (rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_UP)
                rd_kafka_LeaveGroupRequest(rkcg->rkcg_rkb, rkcg->rkcg_group_id,
                                           rkcg->rkcg_member_id,
					   ignore_response ?
					   RD_KAFKA_NO_REPLYQ :
                                           RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
                                           ignore_response ? NULL :
                                           rd_kafka_handle_LeaveGroup, rkcg);
        else if (!ignore_response)
                rd_kafka_handle_LeaveGroup(rkcg->rkcg_rk, rkcg->rkcg_rkb,
                                           RD_KAFKA_RESP_ERR__WAIT_COORD,
                                           NULL, NULL, rkcg);
}


/**
 * Enqueue a rebalance op (if configured). 'partitions' is copied.
 * This delegates the responsibility of assign() and unassign() to the
 * application.
 *
 * Returns 1 if a rebalance op was enqueued, else 0.
 * Returns 0 if there was no rebalance_cb or 'assignment' is NULL,
 * in which case rd_kafka_cgrp_assign(rkcg,assignment) is called immediately.
 */
static int
rd_kafka_rebalance_op (rd_kafka_cgrp_t *rkcg,
		       rd_kafka_resp_err_t err,
		       rd_kafka_topic_partition_list_t *assignment,
		       const char *reason) {
	rd_kafka_op_t *rko;

	/* Pause current partition set consumers until new assign() is called */
	if (rkcg->rkcg_assignment)
		rd_kafka_toppars_pause_resume(rkcg->rkcg_rk, 1,
					      RD_KAFKA_TOPPAR_F_LIB_PAUSE,
					      rkcg->rkcg_assignment);

	if (!(rkcg->rkcg_rk->rk_conf.enabled_events & RD_KAFKA_EVENT_REBALANCE)
	    || !assignment) {
	no_delegation:
		if (err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS)
			rd_kafka_cgrp_assign(rkcg, assignment);
		else
			rd_kafka_cgrp_unassign(rkcg);
		return 0;
	}

	rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "ASSIGN",
		     "Group \"%s\": delegating %s of %d partition(s) "
		     "to application rebalance callback on queue %s: %s",
		     rkcg->rkcg_group_id->str,
		     err == RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS ?
		     "revoke":"assign", assignment->cnt,
		     rd_kafka_q_dest_name(rkcg->rkcg_q), reason);

	rd_kafka_cgrp_set_join_state(
		rkcg,
		err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS ?
		RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_REBALANCE_CB :
		RD_KAFKA_CGRP_JOIN_STATE_WAIT_REVOKE_REBALANCE_CB);

	rko = rd_kafka_op_new(RD_KAFKA_OP_REBALANCE);
	rko->rko_err = err;
	rko->rko_u.rebalance.partitions =
		rd_kafka_topic_partition_list_copy(assignment);

	if (rd_kafka_q_enq(rkcg->rkcg_q, rko) == 0) {
		/* Queue disabled, handle assignment here. */
		goto no_delegation;
	}

	return 1;
}



static void rd_kafka_cgrp_join (rd_kafka_cgrp_t *rkcg) {
	int metadata_age;

        if (rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP ||
            rkcg->rkcg_join_state != RD_KAFKA_CGRP_JOIN_STATE_INIT)
                return;

	rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "JOIN",
                     "Group \"%.*s\": join with %d (%d) subscribed topic(s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_list_cnt(rkcg->rkcg_subscribed_topics),
		     rkcg->rkcg_subscription->cnt);


	/* We need up-to-date full metadata to continue.
	 * The +1000 is since metadata.refresh.interval.ms can be set to 0. */
	metadata_age = rkcg->rkcg_rk->rk_ts_full_metadata ?
		(int)(rd_clock() - rkcg->rkcg_rk->rk_ts_full_metadata)/1000 :-1;
	if (metadata_age == -1 ||
	    metadata_age >
	    rkcg->rkcg_rk->rk_conf.metadata_refresh_interval_ms + 1000) {
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "JOIN",
			     "Group \"%.*s\": "
			     "postponing join until full metadata is available"
			     " (current metadata age %dms > "
			     "metadata.max.age.ms %dms)",
			     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
			     metadata_age,
			     rkcg->rkcg_rk->rk_conf.
			     metadata_refresh_interval_ms);

		/* Trigger metadata request */
		rd_kafka_metadata0(rkcg->rkcg_rk, 1 /* all topics */, NULL,
				   RD_KAFKA_NO_REPLYQ, "consumer join");
		return;
	}

	if (rd_list_empty(rkcg->rkcg_subscribed_topics)) {
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "JOIN",
			     "Group \"%.*s\": "
			     "no matching topics based on %dms old metadata: "
			     "next metadata refresh in %dms",
			     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
			     metadata_age,
			     rkcg->rkcg_rk->rk_conf.
			     metadata_refresh_interval_ms - metadata_age);
		return;
	}

        rd_kafka_cgrp_set_join_state(rkcg, RD_KAFKA_CGRP_JOIN_STATE_WAIT_JOIN);
        rd_kafka_JoinGroupRequest(rkcg->rkcg_rkb, rkcg->rkcg_group_id,
                                  rkcg->rkcg_member_id,
                                  rkcg->rkcg_rk->rk_conf.group_protocol_type,
                                  rkcg->rkcg_subscribed_topics,
                                  RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
                                  rd_kafka_cgrp_handle_JoinGroup, rkcg);
}

/**
 * Rejoin group on update to effective subscribed topics list
 */
static void rd_kafka_cgrp_rejoin (rd_kafka_cgrp_t *rkcg) {
        /*
         * Clean-up group leader duties, if any.
         */
        rd_kafka_cgrp_group_leader_reset(rkcg);

        /* Remove assignment (async), if any. If there is already an
         * unassign in progress we dont need to bother. */
        if (rkcg->rkcg_assignment) {
		if (!(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_UNASSIGN)) {
			rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_WAIT_UNASSIGN;

			rd_kafka_rebalance_op(
				rkcg,
				RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS,
				rkcg->rkcg_assignment, "unsubscribe");
		}
	} else {
		rd_kafka_cgrp_join(rkcg);
	}
}

/**
 * Update the effective list of subscribed topics and trigger a rejoin
 * if it changed.
 *
 * Set \p topics to NULL for clearing the list.
 *
 * @returns 1 on change, else 0.
 *
 * @remark Takes ownership of \p topics
 */
static int rd_kafka_cgrp_update_subscribed_topics (rd_kafka_cgrp_t *rkcg,
						   rd_list_t *topics) {
	rd_kafka_topic_info_t *tinfo;
	int i;

	if (!topics) {
		if (!rd_list_empty(rkcg->rkcg_subscribed_topics))
			rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "SUBSCRIPTION",
				     "Group \"%.*s\": "
				     "clearing subscribed topics list (%d)",
				     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
				     rd_list_cnt(rkcg->rkcg_subscribed_topics));
		topics = rd_list_new(0);

	} else if (rd_list_cnt(topics) == 0)
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "SUBSCRIPTION",
			     "Group \"%.*s\": "
			     "no topics in metadata matched subscription",
			     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id));

	/* Sort for comparison */
	rd_list_sort(topics, rd_kafka_topic_info_cmp);

	/* Compare to existing to see if anything changed. */
	if (!rd_list_cmp(rkcg->rkcg_subscribed_topics, topics,
			 rd_kafka_topic_info_cmp)) {
		/* No change */
		rd_list_destroy(topics, (void *)rd_kafka_topic_info_destroy);
		return 0;
	}

	rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "SUBSCRIPTION",
		     "Group \"%.*s\": effective subscription list changed "
		     "from %d to %d topic(s):",
		     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
		     rd_list_cnt(rkcg->rkcg_subscribed_topics),
		     rd_list_cnt(topics));

	RD_LIST_FOREACH(tinfo, topics, i)
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "SUBSCRIPTION",
			     " Topic %s with %d partition(s)",
			     tinfo->topic, tinfo->partition_cnt);

	rd_list_destroy(rkcg->rkcg_subscribed_topics,
			(void *)rd_kafka_topic_info_destroy);

	rkcg->rkcg_subscribed_topics = topics;

	return 1;
}




static void rd_kafka_cgrp_heartbeat (rd_kafka_cgrp_t *rkcg,
                                     rd_kafka_broker_t *rkb) {
        /* Skip heartbeat if we have one in transit */
        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT)
                return;

        rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_HEARTBEAT_IN_TRANSIT;
        rd_kafka_HeartbeatRequest(rkb, rkcg->rkcg_group_id,
                                  rkcg->rkcg_generation_id,
                                  rkcg->rkcg_member_id,
                                  RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
                                  rd_kafka_cgrp_handle_Heartbeat, rkcg);
}

/**
 * Cgrp is now terminated: decommission it and signal back to application.
 */
static void rd_kafka_cgrp_terminated (rd_kafka_cgrp_t *rkcg) {

	rd_kafka_assert(NULL, rkcg->rkcg_wait_unassign_cnt == 0);
	rd_kafka_assert(NULL, rkcg->rkcg_wait_commit_cnt == 0);
	rd_kafka_assert(NULL, !(rkcg->rkcg_flags&RD_KAFKA_CGRP_F_WAIT_UNASSIGN));
        rd_kafka_assert(NULL, rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_TERM);

        rd_kafka_timer_stop(&rkcg->rkcg_rk->rk_timers,
                            &rkcg->rkcg_offset_commit_tmr, 1/*lock*/);

	rd_kafka_q_purge(rkcg->rkcg_wait_coord_q);

	/* Disable and empty ops queue since there will be no
	 * (broker) thread serving it anymore after the unassign_broker
	 * below.
	 * This prevents hang on destroy where responses are enqueued on rkcg_ops
	 * without anything serving the queue. */
	rd_kafka_q_disable(rkcg->rkcg_ops);
	rd_kafka_q_purge(rkcg->rkcg_ops);

	if (rkcg->rkcg_rkb)
		rd_kafka_cgrp_unassign_broker(rkcg);

        if (rkcg->rkcg_reply_rko) {
                /* Signal back to application. */
                rd_kafka_replyq_enq(&rkcg->rkcg_reply_rko->rko_replyq,
				    rkcg->rkcg_reply_rko, 0);
                rkcg->rkcg_reply_rko = NULL;
        }
}


/**
 * If a cgrp is terminating and all outstanding ops are now finished
 * then progress to final termination and return 1.
 * Else returns 0.
 */
static RD_INLINE int rd_kafka_cgrp_try_terminate (rd_kafka_cgrp_t *rkcg) {

        if (rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_TERM)
                return 1;

	if (likely(!(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)))
		return 0;

	/* Check if wait-coord queue has timed out. */
	if (rd_kafka_q_len(rkcg->rkcg_wait_coord_q) > 0 &&
	    rkcg->rkcg_ts_terminate +
	    (rkcg->rkcg_rk->rk_conf.group_session_timeout_ms * 1000) <
	    rd_clock()) {
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPTERM",
			     "Group \"%s\": timing out %d op(s) in "
			     "wait-for-coordinator queue",
			     rkcg->rkcg_group_id->str,
			     rd_kafka_q_len(rkcg->rkcg_wait_coord_q));
		rd_kafka_q_disable(rkcg->rkcg_wait_coord_q);
		if (rd_kafka_q_concat(rkcg->rkcg_ops,
				      rkcg->rkcg_wait_coord_q) == -1) {
			/* ops queue shut down, purge coord queue */
			rd_kafka_q_purge(rkcg->rkcg_wait_coord_q);
		}
	}

	if (!RD_KAFKA_CGRP_WAIT_REBALANCE_CB(rkcg) &&
	    rd_list_empty(&rkcg->rkcg_toppars) &&
	    rkcg->rkcg_wait_unassign_cnt == 0 &&
	    rkcg->rkcg_wait_commit_cnt == 0 &&
            !(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_UNASSIGN)) {
                /* Since we might be deep down in a 'rko' handler
                 * called from cgrp_op_serve() we cant call terminated()
                 * directly since it will decommission the rkcg_ops queue
                 * that might be locked by intermediate functions.
                 * Instead set the TERM state and let the cgrp terminate
                 * at its own discretion. */
                rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_TERM);
                return 1;
        } else {
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPTERM",
			     "Group \"%s\": "
			     "waiting for %d toppar(s), %d unassignment(s), "
			     "%d commit(s)%s (state %s, join-state %s) "
			     "before terminating",
			     rkcg->rkcg_group_id->str,
			     rd_list_cnt(&rkcg->rkcg_toppars),
			     rkcg->rkcg_wait_unassign_cnt,
			     rkcg->rkcg_wait_commit_cnt,
			     (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_UNASSIGN)?
			     ", wait-unassign flag," : "",
			     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
			     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);
                return 0;
        }
}


/**
 * Add partition to this cgrp management
 */
static void rd_kafka_cgrp_partition_add (rd_kafka_cgrp_t *rkcg,
                                         rd_kafka_toppar_t *rktp) {
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP,"PARTADD",
                     "Group \"%s\": add %s [%"PRId32"]",
                     rkcg->rkcg_group_id->str,
                     rktp->rktp_rkt->rkt_topic->str,
                     rktp->rktp_partition);

        rd_kafka_assert(rkcg->rkcg_rk, !rktp->rktp_s_for_cgrp);
        rktp->rktp_s_for_cgrp = rd_kafka_toppar_keep(rktp);
        rd_list_add(&rkcg->rkcg_toppars, rktp->rktp_s_for_cgrp);
}

/**
 * Remove partition from this cgrp management
 */
static void rd_kafka_cgrp_partition_del (rd_kafka_cgrp_t *rkcg,
                                         rd_kafka_toppar_t *rktp) {
        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "PARTDEL",
                     "Group \"%s\": delete %s [%"PRId32"]",
                     rkcg->rkcg_group_id->str,
                     rktp->rktp_rkt->rkt_topic->str,
                     rktp->rktp_partition);
        rd_kafka_assert(rkcg->rkcg_rk, rktp->rktp_s_for_cgrp);

        rd_list_remove(&rkcg->rkcg_toppars, rktp->rktp_s_for_cgrp);
        rd_kafka_toppar_destroy(rktp->rktp_s_for_cgrp);
        rktp->rktp_s_for_cgrp = NULL;

        rd_kafka_cgrp_try_terminate(rkcg);
}



/**
 * Reply for OffsetFetch from call below.
 */
static void rd_kafka_cgrp_offsets_fetch_response (
	rd_kafka_t *rk,
	rd_kafka_broker_t *rkb,
	rd_kafka_resp_err_t err,
	rd_kafka_buf_t *reply,
	rd_kafka_buf_t *request,
	void *opaque) {
	rd_kafka_topic_partition_list_t *offsets = opaque;
	rd_kafka_cgrp_t *rkcg;

	if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* Termination, quick cleanup. */
		rd_kafka_topic_partition_list_destroy(offsets);
                return;
        }

	rkcg = rd_kafka_cgrp_get(rk);

	rd_kafka_topic_partition_list_log(rk, "OFFSETFETCH", offsets);
	/* If all partitions already had usable offsets then there
	 * was no request sent and thus no reply, the offsets list is
	 * good to go. */
	if (reply)
		err = rd_kafka_handle_OffsetFetch(rk, rkb, err,
						  reply, request, offsets,
						  1/* Update toppars */);
	if (err) {
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "OFFSET",
			     "Offset fetch error: %s",
			     rd_kafka_err2str(err));

		if (err != RD_KAFKA_RESP_ERR__WAIT_COORD)
			rd_kafka_q_op_err(rkcg->rkcg_q,
					  RD_KAFKA_OP_CONSUMER_ERR, err, 0,
					  NULL, 0,
					  "Failed to fetch offsets: %s",
					  rd_kafka_err2str(err));
	} else {
		if (RD_KAFKA_CGRP_CAN_FETCH_START(rkcg))
			rd_kafka_cgrp_partitions_fetch_start(
				rkcg, offsets, 1 /* usable offsets */);
		else
			rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "OFFSET",
				     "Group \"%.*s\": "
				     "ignoring Offset fetch response for "
				     "%d partition(s): in state %s",
				     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
				     offsets ? offsets->cnt : -1,
				     rd_kafka_cgrp_join_state_names[
					     rkcg->rkcg_join_state]);
	}

	rd_kafka_topic_partition_list_destroy(offsets);
}

/**
 * Fetch offsets for a list of partitions
 */
static void
rd_kafka_cgrp_offsets_fetch (rd_kafka_cgrp_t *rkcg, rd_kafka_broker_t *rkb,
                             rd_kafka_topic_partition_list_t *offsets) {
	rd_kafka_topic_partition_list_t *use_offsets;

	/* Make a copy of the offsets */
	use_offsets = rd_kafka_topic_partition_list_copy(offsets);

        if (rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP || !rkb)
		rd_kafka_cgrp_offsets_fetch_response(
			rkcg->rkcg_rk, rkb, RD_KAFKA_RESP_ERR__WAIT_COORD,
			NULL, NULL, use_offsets);
        else {
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "OFFSET",
			     "Fetch %d offsets with v%d",
			     use_offsets->cnt, rkcg->rkcg_version);
                rd_kafka_OffsetFetchRequest(
                        rkb, 1, offsets,
                        RD_KAFKA_REPLYQ(rkcg->rkcg_ops, rkcg->rkcg_version),
			rd_kafka_cgrp_offsets_fetch_response,
			use_offsets);
	}

}


/**
 * Start fetching all partitions in 'assignment' (async)
 */
static void
rd_kafka_cgrp_partitions_fetch_start0 (rd_kafka_cgrp_t *rkcg,
				       rd_kafka_topic_partition_list_t
				       *assignment, int usable_offsets,
				       int line) {
        int i;

	/* If waiting for offsets to commit we need that to finish first
	 * before starting fetchers (which might fetch those stored offsets).*/
	if (rkcg->rkcg_wait_commit_cnt > 0) {
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "FETCHSTART",
			     "Group \"%s\": not starting fetchers "
			     "for %d assigned partition(s) in join-state %s "
			     "(usable_offsets=%s, v%"PRId32", line %d): "
			     "waiting for %d commit(s)",
			     rkcg->rkcg_group_id->str, assignment->cnt,
			     rd_kafka_cgrp_join_state_names[rkcg->
							    rkcg_join_state],
			     usable_offsets ? "yes":"no",
			     rkcg->rkcg_version, line,
			     rkcg->rkcg_wait_commit_cnt);
		return;
	}

	rd_kafka_cgrp_version_new_barrier(rkcg);

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "FETCHSTART",
                     "Group \"%s\": starting fetchers for %d assigned "
                     "partition(s) in join-state %s "
		     "(usable_offsets=%s, v%"PRId32", line %d)",
                     rkcg->rkcg_group_id->str, assignment->cnt,
		     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
		     usable_offsets ? "yes":"no",
		     rkcg->rkcg_version, line);

	rd_kafka_topic_partition_list_log(rkcg->rkcg_rk,
					  "FETCHSTART", assignment);

        if (assignment->cnt == 0)
                return;

	/* Check if offsets are really unusable, this is to catch the
	 * case where the entire assignment has absolute offsets set which
	 * should make us skip offset lookups. */
	if (!usable_offsets)
		usable_offsets =
			rd_kafka_topic_partition_list_count_abs_offsets(
				assignment) == assignment->cnt;

        if (!usable_offsets &&
            rkcg->rkcg_rk->rk_conf.offset_store_method ==
            RD_KAFKA_OFFSET_METHOD_BROKER) {

                /* Fetch offsets for all assigned partitions */
                rd_kafka_cgrp_offsets_fetch(rkcg, rkcg->rkcg_rkb, assignment);

        } else {
		rd_kafka_cgrp_set_join_state(rkcg,
					     RD_KAFKA_CGRP_JOIN_STATE_STARTED);

                for (i = 0 ; i < assignment->cnt ; i++) {
                        rd_kafka_topic_partition_t *rktpar =
                                &assignment->elems[i];
                        shptr_rd_kafka_toppar_t *s_rktp = rktpar->_private;
                        rd_kafka_toppar_t *rktp = rd_kafka_toppar_s2i(s_rktp);

			if (!rktp->rktp_assigned) {
				rktp->rktp_assigned = 1;
				rkcg->rkcg_assigned_cnt++;

				/* Start fetcher for partition and
				 * forward partition's fetchq to
				 * consumer groups queue. */
				rd_kafka_toppar_op_fetch_start(
					rktp, rktpar->offset,
					rkcg->rkcg_q, RD_KAFKA_NO_REPLYQ);
			} else {
				int64_t offset;
				/* Fetcher already started,
				 * just do seek to update offset */
				rd_kafka_toppar_lock(rktp);
				if (rktpar->offset < rktp->rktp_app_offset)
					offset = rktp->rktp_app_offset;
				else
					offset = rktpar->offset;
				rd_kafka_toppar_unlock(rktp);
				rd_kafka_toppar_op_seek(rktp, offset,
							RD_KAFKA_NO_REPLYQ);
			}
                }
        }

	rd_kafka_assert(NULL, rkcg->rkcg_assigned_cnt <=
			(rkcg->rkcg_assignment ? rkcg->rkcg_assignment->cnt : 0));
}






/**
 * Handler of OffsetCommit response (after parsing).
 * @remark \p offsets may be NULL if \p err is set
 */
static void
rd_kafka_cgrp_handle_OffsetCommit (rd_kafka_cgrp_t *rkcg,
                                   rd_kafka_resp_err_t err,
                                   rd_kafka_topic_partition_list_t
                                   *offsets) {
	int i;

	if (!err) {
		/* Update toppars' committed offset */
		for (i = 0 ; i < offsets->cnt ; i++) {
			rd_kafka_topic_partition_t *rktpar =&offsets->elems[i];
			shptr_rd_kafka_toppar_t *s_rktp;
			rd_kafka_toppar_t *rktp;

			if (unlikely(rktpar->err)) {
				rd_kafka_dbg(rkcg->rkcg_rk, TOPIC,
					     "OFFSET",
					     "OffsetCommit failed for "
					     "%s [%"PRId32"] at offset "
					     "%"PRId64": %s",
					     rktpar->topic, rktpar->partition,
					     rktpar->offset,
					     rd_kafka_err2str(rktpar->err));
				continue;
			} else if (unlikely(rktpar->offset < 0))
				continue;

			s_rktp = rd_kafka_topic_partition_list_get_toppar(
				rkcg->rkcg_rk, offsets, i);
			if (!s_rktp)
				continue;

			rktp = rd_kafka_toppar_s2i(s_rktp);
			rd_kafka_toppar_lock(rktp);
			rktp->rktp_committed_offset = rktpar->offset;
			rd_kafka_toppar_unlock(rktp);

			rd_kafka_toppar_destroy(s_rktp);
		}
	}

        if (rd_kafka_cgrp_try_terminate(rkcg))
                return; /* terminated */

        if (rkcg->rkcg_join_state == RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN)
		rd_kafka_cgrp_check_unassign_done(rkcg,
                                                  "OffsetCommit done");
}




/**
 * Handle OffsetCommitResponse
 * Takes the original 'rko' as opaque argument.
 * @remark \p rkb, rkbuf, and request may be NULL in a number of
 *         error cases (e.g., _NO_OFFSET, _WAIT_COORD)
 */
static void rd_kafka_cgrp_op_handle_OffsetCommit (rd_kafka_t *rk,
						  rd_kafka_broker_t *rkb,
						  rd_kafka_resp_err_t err,
						  rd_kafka_buf_t *rkbuf,
						  rd_kafka_buf_t *request,
						  void *opaque) {
	rd_kafka_cgrp_t *rkcg = rk->rk_cgrp;
        rd_kafka_op_t *rko_orig = opaque;
	rd_kafka_topic_partition_list_t *offsets =
		rko_orig->rko_u.offset_commit.partitions; /* maybe NULL */

	RD_KAFKA_OP_TYPE_ASSERT(rko_orig, RD_KAFKA_OP_OFFSET_COMMIT);

	err = rd_kafka_handle_OffsetCommit(rk, rkb, err, rkbuf,
					   request, offsets);

	if (err == RD_KAFKA_RESP_ERR__IN_PROGRESS)
		return; /* Retrying */

	rd_kafka_assert(NULL, rkcg->rkcg_wait_commit_cnt > 0);
	rkcg->rkcg_wait_commit_cnt--;
        /* check_unassign_done() is called from handle_OffsetCommit() below */

	if (err == RD_KAFKA_RESP_ERR__DESTROY) {
		rd_kafka_op_destroy(rko_orig);
                rd_kafka_cgrp_check_unassign_done(rkcg,
                                                  "OffsetCommit done (__DESTROY)");
		return;
	}

	/* If no special callback is set but a offset_commit_cb has
	 * been set in conf then post an event for the latter. */
	if (!rko_orig->rko_u.offset_commit.cb && rk->rk_conf.offset_commit_cb) {
                rd_kafka_op_t *rko_reply = rd_kafka_op_new_reply(rko_orig, err);

		if (offsets)
			rko_reply->rko_u.offset_commit.partitions =
				rd_kafka_topic_partition_list_copy(offsets);

		rko_reply->rko_u.offset_commit.cb =
			rk->rk_conf.offset_commit_cb;
		rko_reply->rko_u.offset_commit.opaque = rk->rk_conf.opaque;

                rd_kafka_q_enq(rk->rk_rep, rko_reply);
	}


	/* Enqueue reply to requester's queue, if any. */
	if (rko_orig->rko_replyq.q) {
                rd_kafka_op_t *rko_reply = rd_kafka_op_new_reply(rko_orig, err);

		/* Copy offset & partitions & callbacks to reply op */
		rko_reply->rko_u.offset_commit = rko_orig->rko_u.offset_commit;
		if (offsets)
			rko_reply->rko_u.offset_commit.partitions =
				rd_kafka_topic_partition_list_copy(offsets);

                rd_kafka_replyq_enq(&rko_orig->rko_replyq, rko_reply, 0);
        }

	rd_kafka_cgrp_handle_OffsetCommit(rkcg, err, offsets);

        rd_kafka_op_destroy(rko_orig);
}


/**
 * Commit a list of offsets.
 * Reuse the orignating 'rko' for the async reply.
 * 'rko->rko_payload' should either by NULL (to commit current assignment) or
 * a proper topic_partition_list_t with offsets to commit.
 * The offset list will be altered.
 *
 * \p silent_empty: if there are no offsets to commit bail out silently without
 *                  posting an op on the reply queue.
 *
 * Locality: cgrp thread
 */
static void rd_kafka_cgrp_offsets_commit (rd_kafka_cgrp_t *rkcg,
                                          rd_kafka_op_t *rko) {
	rd_kafka_topic_partition_list_t *offsets;
	rd_kafka_resp_err_t err;
	int silent_empty = rko->rko_u.offset_commit.silent_empty;

	/* If offsets is NULL we shall use the current assignment. */
	if (!rko->rko_u.offset_commit.partitions && rkcg->rkcg_assignment) {
		/* Copy assignment and fill in offsets */
		rko->rko_u.offset_commit.partitions =
			rd_kafka_topic_partition_list_copy(
				rkcg->rkcg_assignment);

		rd_kafka_topic_partition_list_set_offsets(
			rkcg->rkcg_rk, rko->rko_u.offset_commit.partitions, 1,
			RD_KAFKA_OFFSET_INVALID/* def */,
			1 /* is commit */);

	}

	offsets = rko->rko_u.offset_commit.partitions;

	if (!offsets && silent_empty) {
		rd_kafka_op_destroy(rko);
		return;
	}

	/* Reprocessing ops will have increased wait_commit_cnt already. */
	if (!(rko->rko_flags & RD_KAFKA_OP_F_REPROCESS))
		rkcg->rkcg_wait_commit_cnt++;


	if (!offsets || offsets->cnt == 0) {
		err = RD_KAFKA_RESP_ERR__NO_OFFSET;
		goto err;
	}

        if (rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP || !rkcg->rkcg_rkb ||
	    rkcg->rkcg_rkb->rkb_source == RD_KAFKA_INTERNAL) {
		/* wait_coord_q is disabled session.timeout.ms after
		 * group close() has been initated. */
		if (rko->rko_u.offset_commit.ts_timeout == 0 &&
                    rd_kafka_q_ready(rkcg->rkcg_wait_coord_q)) {
			rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "COMMIT",
				     "Group \"%s\": "
				     "unable to OffsetCommit in state %s: "
				     "coordinator (%s) is unavailable: "
				     "retrying later",
				     rkcg->rkcg_group_id->str,
				     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
				     rkcg->rkcg_rkb ?
				     rd_kafka_broker_name(rkcg->rkcg_rkb) :
				     "none");
			rko->rko_flags |= RD_KAFKA_OP_F_REPROCESS;
                        rko->rko_u.offset_commit.ts_timeout = rd_clock() +
                                (rkcg->rkcg_rk->rk_conf.group_session_timeout_ms
                                 * 1000);
			rd_kafka_q_enq(rkcg->rkcg_wait_coord_q, rko);
			return;
		}

		err = RD_KAFKA_RESP_ERR__WAIT_COORD;

	} else if (rd_kafka_OffsetCommitRequest(
			   rkcg->rkcg_rkb, rkcg, 1, offsets,
			   RD_KAFKA_REPLYQ(rkcg->rkcg_ops, rkcg->rkcg_version),
			   rd_kafka_cgrp_op_handle_OffsetCommit, rko) != 0)
		return;
	else
		err = RD_KAFKA_RESP_ERR__NO_OFFSET;

 err:
	/* No valid offsets */
	if (silent_empty) {
                if (!(rko->rko_flags & RD_KAFKA_OP_F_REPROCESS)) {
                        rkcg->rkcg_wait_commit_cnt--;
                        /* Must not call check_unassign_done() here
                         * for silent empty offset commits. */
                }
		rd_kafka_op_destroy(rko);
		return;
	}

	/* Propagate error to whoever wanted offset committed. */
	rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "COMMIT",
		     "OffsetCommit internal error: %s", rd_kafka_err2str(err));
	rd_kafka_cgrp_op_handle_OffsetCommit(rkcg->rkcg_rk, NULL, err,
					     NULL, NULL, rko);
}


/**
 * Commit offsets for all assigned partitions.
 */
static void rd_kafka_cgrp_assigned_offsets_commit (rd_kafka_cgrp_t *rkcg) {
        rd_kafka_op_t *rko;

	rko = rd_kafka_op_new(RD_KAFKA_OP_OFFSET_COMMIT);
	if (rkcg->rkcg_rk->rk_conf.enabled_events & RD_KAFKA_EVENT_OFFSET_COMMIT) {
		rd_kafka_op_set_replyq(rko, rkcg->rkcg_rk->rk_rep, 0);
		rko->rko_u.offset_commit.cb =
			rkcg->rkcg_rk->rk_conf.offset_commit_cb; /*maybe NULL*/
		rko->rko_u.offset_commit.opaque = rkcg->rkcg_rk->rk_conf.opaque;
	}
        /* NULL partitions means current assignment */
	rko->rko_u.offset_commit.silent_empty = 1;
        rd_kafka_cgrp_offsets_commit(rkcg, rko);
}


/**
 * auto.commit.interval.ms commit timer callback.
 *
 * Trigger a group offset commit.
 *
 * Locality: rdkafka main thread
 */
static void rd_kafka_cgrp_offset_commit_tmr_cb (rd_kafka_timers_t *rkts,
                                                void *arg) {
        rd_kafka_cgrp_t *rkcg = arg;

	rd_kafka_cgrp_assigned_offsets_commit(rkcg);
}




/**
 * Call when all unassign operations are done to transition to the next state
 */
static void rd_kafka_cgrp_unassign_done (rd_kafka_cgrp_t *rkcg,
                                         const char *reason) {
	rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "UNASSIGN",
		     "Group \"%s\": unassign done in state %s (join state %s): "
		     "%s: %s",
		     rkcg->rkcg_group_id->str,
		     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
		     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
		     rkcg->rkcg_assignment ?
		     "with new assignment" : "without new assignment",
                     reason);

	if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN) {
		rd_kafka_cgrp_leave(rkcg, 1/*ignore response*/);
		rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN;
	}

        if (rkcg->rkcg_assignment) {
		rd_kafka_cgrp_set_join_state(rkcg,
					     RD_KAFKA_CGRP_JOIN_STATE_ASSIGNED);
                if (RD_KAFKA_CGRP_CAN_FETCH_START(rkcg))
                        rd_kafka_cgrp_partitions_fetch_start(
                                rkcg, rkcg->rkcg_assignment, 0);
	} else {
		rd_kafka_cgrp_set_join_state(rkcg,
					     RD_KAFKA_CGRP_JOIN_STATE_INIT);
	}

	rd_kafka_cgrp_try_terminate(rkcg);
}


/**
 * Checks if the current unassignment is done and if so
 * calls .._done().
 * Else does nothing.
 */
static void rd_kafka_cgrp_check_unassign_done (rd_kafka_cgrp_t *rkcg,
                                               const char *reason) {
	if (rkcg->rkcg_wait_unassign_cnt > 0 ||
	    rkcg->rkcg_assigned_cnt > 0 ||
	    rkcg->rkcg_wait_commit_cnt > 0 ||
	    rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_UNASSIGN) {
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "UNASSIGN",
                             "Unassign not done yet "
                             "(%d wait_unassign, %d assigned, %d wait commit"
                             "%s)",
                             rkcg->rkcg_wait_unassign_cnt,
                             rkcg->rkcg_assigned_cnt,
                             rkcg->rkcg_wait_commit_cnt,
                             (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_UNASSIGN)?
                             ", F_WAIT_UNASSIGN" : "");
		return;
        }

	rd_kafka_cgrp_unassign_done(rkcg, reason);
}



/**
 * Remove existing assignment.
 */
static rd_kafka_resp_err_t
rd_kafka_cgrp_unassign (rd_kafka_cgrp_t *rkcg) {
        int i;

	rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_WAIT_UNASSIGN;

        if (!rkcg->rkcg_assignment) {
		rd_kafka_cgrp_check_unassign_done(rkcg, "unassign");
                return RD_KAFKA_RESP_ERR_NO_ERROR;
	}

	rd_kafka_cgrp_version_new_barrier(rkcg);

	rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "UNASSIGN",
                     "Group \"%s\": unassigning %d partition(s) (v%"PRId32")",
                     rkcg->rkcg_group_id->str, rkcg->rkcg_assignment->cnt,
		     rkcg->rkcg_version);

        rd_kafka_cgrp_set_join_state(rkcg,
                                     RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN);

        if (rkcg->rkcg_rk->rk_conf.offset_store_method ==
            RD_KAFKA_OFFSET_METHOD_BROKER &&
	    rkcg->rkcg_rk->rk_conf.enable_auto_commit) {
                /* Commit all offsets for all assigned partitions to broker */
                rd_kafka_cgrp_assigned_offsets_commit(rkcg);
        }


        for (i = 0 ; i < rkcg->rkcg_assignment->cnt ; i++) {
                rd_kafka_topic_partition_t *rktpar;
                shptr_rd_kafka_toppar_t *s_rktp;
                rd_kafka_toppar_t *rktp;

                rktpar = &rkcg->rkcg_assignment->elems[i];
                s_rktp = rktpar->_private;
                rktp = rd_kafka_toppar_s2i(s_rktp);

                if (rktp->rktp_assigned) {
                        rd_kafka_toppar_op_fetch_stop(
				rktp, RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0));
                        rkcg->rkcg_wait_unassign_cnt++;
                }

                rd_kafka_toppar_lock(rktp);
                rd_kafka_toppar_desired_del(rktp);
                rd_kafka_toppar_unlock(rktp);
        }

	/* Resume partition consumption. */
	rd_kafka_toppars_pause_resume(rkcg->rkcg_rk, 0/*resume*/,
				      RD_KAFKA_TOPPAR_F_LIB_PAUSE,
				      rkcg->rkcg_assignment);

        rd_kafka_topic_partition_list_destroy(rkcg->rkcg_assignment);
        rkcg->rkcg_assignment = NULL;

        rd_kafka_cgrp_check_unassign_done(rkcg, "unassign#2");

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * Set new atomic partition assignment
 * May update \p assignment but will not hold on to it.
 */
static void
rd_kafka_cgrp_assign (rd_kafka_cgrp_t *rkcg,
                      rd_kafka_topic_partition_list_t *assignment) {
        int i;

        /* FIXME: Make a diff of what partitions are removed and added. */

        /* Get toppar object for each partition */
        for (i = 0 ; assignment && i < assignment->cnt ; i++) {
                rd_kafka_topic_partition_t *rktpar;
                shptr_rd_kafka_toppar_t *s_rktp;

                rktpar = &assignment->elems[i];

                /* Use existing toppar if set */
                if (rktpar->_private)
                        continue;

                s_rktp = rd_kafka_toppar_get2(rkcg->rkcg_rk,
                                              rktpar->topic,
                                              rktpar->partition,
                                              0/*no-ua*/, 1/*create-on-miss*/);
                if (s_rktp)
                        rktpar->_private = s_rktp;
        }

	rd_kafka_cgrp_version_new_barrier(rkcg);

        /* Remove existing assignment (async operation) */
	if (rkcg->rkcg_assignment)
		rd_kafka_cgrp_unassign(rkcg);

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "ASSIGN",
                     "Group \"%s\": assigning %d partition(s) in join state %s",
                     rkcg->rkcg_group_id->str, assignment ? assignment->cnt : 0,
                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);


	if (rkcg->rkcg_wait_unassign_cnt == 0)
		rd_kafka_cgrp_set_join_state(
			rkcg, RD_KAFKA_CGRP_JOIN_STATE_ASSIGNED);

	if (assignment)
		rkcg->rkcg_assignment =
			rd_kafka_topic_partition_list_copy(assignment);

        if (RD_KAFKA_CGRP_CAN_FETCH_START(rkcg) && rkcg->rkcg_assignment) {
                /* No existing assignment that needs to be decommissioned,
                 * start partition fetchers right away */
                rd_kafka_cgrp_partitions_fetch_start(
                        rkcg, rkcg->rkcg_assignment, 0);
        }
}




/**
 * Handle a rebalance-triggered partition assignment.
 *
 * If a rebalance_cb has been registered we enqueue an op for the app
 * and let the app perform the actual assign() call.
 * Otherwise we assign() directly from here.
 *
 * This provides the most flexibility, allowing the app to perform any
 * operation it seem fit (e.g., offset writes or reads) before actually
 * updating the assign():ment.
 */
static void
rd_kafka_cgrp_handle_assignment (rd_kafka_cgrp_t *rkcg,
				 rd_kafka_topic_partition_list_t *assignment) {

	rd_kafka_rebalance_op(rkcg, RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS,
			      assignment, "new assignment");
}


/**
 * Handle HeartbeatResponse errors.
 *
 * If an IllegalGeneration error code is returned in the
 * HeartbeatResponse, it indicates that the co-ordinator has
 * initiated a rebalance. The consumer then stops fetching data,
 * commits offsets and sends a JoinGroupRequest to it's co-ordinator
 * broker */
void rd_kafka_cgrp_handle_heartbeat_error (rd_kafka_cgrp_t *rkcg,
					   rd_kafka_resp_err_t err) {


	rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
		     "Group \"%s\" heartbeat error response in "
		     "state %s (join state %s, %d partition(s) assigned): %s",
		     rkcg->rkcg_group_id->str,
		     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
		     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
		     rkcg->rkcg_assignment ? rkcg->rkcg_assignment->cnt : 0,
		     rd_kafka_err2str(err));

	if (rkcg->rkcg_join_state <= RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC) {
		rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "HEARTBEAT",
			     "Heartbeat response: discarding outdated "
			     "request (now in join-state %s)",
			     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);
		return;
	}

	switch (err)
	{
	case RD_KAFKA_RESP_ERR__DESTROY:
		/* quick cleanup */
		break;
	case RD_KAFKA_RESP_ERR_NOT_COORDINATOR_FOR_GROUP:
	case RD_KAFKA_RESP_ERR_GROUP_COORDINATOR_NOT_AVAILABLE:
	case RD_KAFKA_RESP_ERR__TRANSPORT:
		/* Remain in joined state and keep querying for coordinator */
		rd_interval_expedite(&rkcg->rkcg_coord_query_intvl, 0);
		break;

	case RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID:
		rd_kafka_cgrp_set_member_id(rkcg, "");
	case RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS:
	case RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION:
	default:
                /* Just revert to INIT state if join state is active. */
                if (rkcg->rkcg_join_state <
                    RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_REBALANCE_CB ||
                    rkcg->rkcg_join_state ==
                    RD_KAFKA_CGRP_JOIN_STATE_WAIT_REVOKE_REBALANCE_CB)
                        break;

                rd_kafka_cgrp_set_join_state(rkcg, RD_KAFKA_CGRP_JOIN_STATE_INIT);

                if (!(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_UNASSIGN)) {
                        rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_WAIT_UNASSIGN;

                        /* Trigger rebalance_cb */
                        rd_kafka_rebalance_op(
                                rkcg, RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS,
                                rkcg->rkcg_assignment, rd_kafka_err2str(err));
                }
                break;
        }
}



/**
 * Clean up any group-leader related resources.
 *
 * Locality: cgrp thread
 */
void rd_kafka_cgrp_group_leader_reset (rd_kafka_cgrp_t *rkcg){
        if (rkcg->rkcg_group_leader.protocol) {
                rd_free(rkcg->rkcg_group_leader.protocol);
                rkcg->rkcg_group_leader.protocol = NULL;
        }

        if (rkcg->rkcg_group_leader.members) {
                int i;

                for (i = 0 ; i < rkcg->rkcg_group_leader.member_cnt ; i++)
                        rd_kafka_group_member_clear(&rkcg->rkcg_group_leader.
                                                    members[i]);
                rkcg->rkcg_group_leader.member_cnt = 0;
                rd_free(rkcg->rkcg_group_leader.members);
                rkcg->rkcg_group_leader.members = NULL;
        }
}




/**
 * Remove existing topic subscription.
 */
static rd_kafka_resp_err_t
rd_kafka_cgrp_unsubscribe (rd_kafka_cgrp_t *rkcg, int leave_group) {

	rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "UNSUBSCRIBE",
		     "Group \"%.*s\": unsubscribe from current %ssubscription "
		     "of %d topics (leave group=%s, join state %s, v%"PRId32")",
		     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
		     rkcg->rkcg_subscription ? "" : "unset ",
		     rkcg->rkcg_subscription ? rkcg->rkcg_subscription->cnt : 0,
		     leave_group ? "yes":"no",
		     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
		     rkcg->rkcg_version);

        if (rkcg->rkcg_subscription) {
                rd_kafka_topic_partition_list_destroy(rkcg->rkcg_subscription);
                rkcg->rkcg_subscription = NULL;
        }

	rd_kafka_cgrp_update_subscribed_topics(rkcg, NULL);

        /*
         * Clean-up group leader duties, if any.
         */
        rd_kafka_cgrp_group_leader_reset(rkcg);

	if (leave_group)
		rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_LEAVE_ON_UNASSIGN;



        /* Remove assignment (async), if any. If there is already an
         * unassign in progress we dont need to bother. */
        if (!(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_UNASSIGN)) {
                rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_WAIT_UNASSIGN;

                rd_kafka_rebalance_op(rkcg,
				      RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS,
				      rkcg->rkcg_assignment, "unsubscribe");
        }

        rkcg->rkcg_flags &= ~RD_KAFKA_CGRP_F_SUBSCRIPTION;

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * Set new atomic topic subscription.
 */
static rd_kafka_resp_err_t
rd_kafka_cgrp_subscribe (rd_kafka_cgrp_t *rkcg,
                         rd_kafka_topic_partition_list_t *rktparlist) {

	rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "UNSUBSCRIBE",
		     "Group \"%.*s\": subscribe to new %ssubscription "
		     "of %d topics (join state %s)",
		     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
		     rktparlist ? "" : "unset ",
		     rktparlist ? rktparlist->cnt : 0,
		     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state]);

        if (rkcg->rkcg_rk->rk_conf.enabled_assignor_cnt == 0)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        /* Remove existing subscription first */
        rd_kafka_cgrp_unsubscribe(rkcg, 0/* dont leave group */);

        if (!rktparlist)
                return RD_KAFKA_RESP_ERR_NO_ERROR;

        rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_SUBSCRIPTION;

        rkcg->rkcg_subscription = rktparlist;

        rd_kafka_cgrp_join(rkcg);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}






/**
 * Same as cgrp_terminate() but called from the cgrp/main thread upon receiving
 * the op 'rko' from cgrp_terminate().
 *
 * NOTE: Takes ownership of 'rko'
 *
 * Locality: main thread
 */
void
rd_kafka_cgrp_terminate0 (rd_kafka_cgrp_t *rkcg, rd_kafka_op_t *rko) {

	rd_kafka_assert(NULL, thrd_is_current(rkcg->rkcg_rk->rk_thread));

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPTERM",
                     "Terminating group \"%.*s\" in state %s "
                     "with %d partition(s)",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                     rd_list_cnt(&rkcg->rkcg_toppars));

        if (unlikely(rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_TERM ||
		     (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE) ||
		     rkcg->rkcg_reply_rko != NULL)) {
                /* Already terminating or handling a previous terminate */
		if (rko) {
			rd_kafka_q_t *rkq = rko->rko_replyq.q;
			rko->rko_replyq.q = NULL;
			rd_kafka_q_op_err(rkq, RD_KAFKA_OP_CONSUMER_ERR,
					  RD_KAFKA_RESP_ERR__IN_PROGRESS,
					  rko->rko_replyq.version,
					  NULL, 0,
					  "Group is %s",
					  rkcg->rkcg_reply_rko ?
					  "terminating":"terminated");
			rd_kafka_q_destroy(rkq);
			rd_kafka_op_destroy(rko);
		}
                return;
        }

        /* Mark for stopping, the actual state transition
         * is performed when all toppars have left. */
        rkcg->rkcg_flags |= RD_KAFKA_CGRP_F_TERMINATE;
	rkcg->rkcg_ts_terminate = rd_clock();
        rkcg->rkcg_reply_rko = rko;

	/* If there's an oustanding rebalance_cb which has not yet been
	 * served by the application it wont be served from now by any poll()
	 * call since the application is calling close().
	 * So we could either wait for close() to serve the queue or simply
	 * perform an unassign call here directly to speed things up.
	 * We choose the latter since the app has already decided to shut down
	 * there is no point in lingering about. */
	if (RD_KAFKA_CGRP_WAIT_REBALANCE_CB(rkcg))
		rd_kafka_cgrp_assign(rkcg, NULL);

        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_SUBSCRIPTION)
                rd_kafka_cgrp_unsubscribe(rkcg, 1/*leave group*/);
        else if (!(rkcg->rkcg_flags & RD_KAFKA_CGRP_F_WAIT_UNASSIGN))
                rd_kafka_cgrp_unassign(rkcg);

        /* Try to terminate right away if all preconditions are met. */
        rd_kafka_cgrp_try_terminate(rkcg);
}


/**
 * Terminate and decommission a cgrp asynchronously.
 *
 * Locality: any thread
 */
void rd_kafka_cgrp_terminate (rd_kafka_cgrp_t *rkcg, rd_kafka_replyq_t replyq) {
	rd_kafka_assert(NULL, !thrd_is_current(rkcg->rkcg_rk->rk_thread));
        rd_kafka_cgrp_op(rkcg, NULL, replyq, RD_KAFKA_OP_TERMINATE, 0);
}


struct _op_timeout_offset_commit {
        rd_ts_t now;
        rd_kafka_t *rk;
        rd_list_t expired;
};

/**
 * q_filter callback for expiring OFFSET_COMMIT timeouts.
 */
static int rd_kafka_op_offset_commit_timeout_check (rd_kafka_q_t *rkq,
                                                    rd_kafka_op_t *rko,
                                                    void *opaque) {
        struct _op_timeout_offset_commit *state =
                (struct _op_timeout_offset_commit*)opaque;

        if (likely(rko->rko_type != RD_KAFKA_OP_OFFSET_COMMIT ||
                   rko->rko_u.offset_commit.ts_timeout == 0 ||
                   rko->rko_u.offset_commit.ts_timeout > state->now)) {
                return 0;
        }

        rd_kafka_q_deq0(rkq, rko);

        /* Add to temporary list to avoid recursive
         * locking of rkcg_wait_coord_q. */
        rd_list_add(&state->expired, rko);
        return 1;
}


/**
 * Scan for various timeouts.
 */
static void rd_kafka_cgrp_timeout_scan (rd_kafka_cgrp_t *rkcg, rd_ts_t now) {
        struct _op_timeout_offset_commit ofc_state;
        int i, cnt = 0;
        rd_kafka_op_t *rko;

        ofc_state.now = now;
        ofc_state.rk = rkcg->rkcg_rk;
        rd_list_init(&ofc_state.expired, 0);

        cnt += rd_kafka_q_apply(rkcg->rkcg_wait_coord_q,
                                rd_kafka_op_offset_commit_timeout_check,
                                &ofc_state);

        RD_LIST_FOREACH(rko, &ofc_state.expired, i)
                rd_kafka_cgrp_op_handle_OffsetCommit(
                        rkcg->rkcg_rk, NULL,
                        RD_KAFKA_RESP_ERR__WAIT_COORD,
                        NULL, NULL, rko);

        rd_list_destroy(&ofc_state.expired, NULL);

        if (cnt > 0)
                rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPTIMEOUT",
                             "Group \"%.*s\": timed out %d op(s), %d remain",
                             RD_KAFKAP_STR_PR(rkcg->rkcg_group_id), cnt,
                             rd_kafka_q_len(rkcg->rkcg_wait_coord_q));


}


/**
 * Serve cgrp op queue.
 */
static void rd_kafka_cgrp_op_serve (rd_kafka_cgrp_t *rkcg,
                                    rd_kafka_broker_t *rkb) {
        rd_kafka_op_t *rko;

        while ((rko = rd_kafka_q_pop(rkcg->rkcg_ops, RD_POLL_NOWAIT,
				     rkcg->rkcg_version))) {
                rd_kafka_toppar_t *rktp = rko->rko_rktp ?
                        rd_kafka_toppar_s2i(rko->rko_rktp) : NULL;
                rd_kafka_resp_err_t err;
                const int silent_op = rko->rko_type == RD_KAFKA_OP_RECV_BUF;

                if (rktp && !silent_op)
                        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPOP",
                                     "Group \"%.*s\" received op %s in state %s "
                                     "(join state %s, v%"PRId32") "
				     "for %.*s [%"PRId32"]",
                                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                                     rd_kafka_op2str(rko->rko_type),
                                     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
				     rkcg->rkcg_version,
                                     RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                                     rktp->rktp_partition);
                else if (!silent_op)
                        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "CGRPOP",
                                     "Group \"%.*s\" received op %s (v%d) in state %s "
                                     "(join state %s, v%"PRId32" vs %"PRId32")",
                                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                                     rd_kafka_op2str(rko->rko_type),
				     rko->rko_version,
                                     rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                                     rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
				     rkcg->rkcg_version, rko->rko_version);

                switch ((int)rko->rko_type)
                {
		case RD_KAFKA_OP_NAME:
			/* Return the currently assigned member id. */
			if (rkcg->rkcg_member_id)
				rko->rko_u.name.str =
					RD_KAFKAP_STR_DUP(rkcg->rkcg_member_id);
			rd_kafka_op_reply(rko, 0);
			rko = NULL;
			break;

                case RD_KAFKA_OP_OFFSET_FETCH:
                        if (rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP ||
                            (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)) {
                                rd_kafka_op_handle_OffsetFetch(
                                        rkcg->rkcg_rk, rkb,
					RD_KAFKA_RESP_ERR__WAIT_COORD,
                                        NULL, NULL, rko);
                                rko = NULL; /* rko freed by handler */
                                break;
                        }

                        rd_kafka_OffsetFetchRequest(
                                rkb, 1,
				rko->rko_u.offset_fetch.partitions,
                                RD_KAFKA_REPLYQ(rkcg->rkcg_ops,
						rkcg->rkcg_version),
                                rd_kafka_op_handle_OffsetFetch, rko);
                        rko = NULL; /* rko now owned by request */
                        break;

                case RD_KAFKA_OP_PARTITION_JOIN:
                        rd_kafka_cgrp_partition_add(rkcg, rktp);

                        /* If terminating tell the partition to leave */
                        if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)
				rd_kafka_toppar_op_fetch_stop(
					rktp, RD_KAFKA_NO_REPLYQ);
                        break;

                case RD_KAFKA_OP_PARTITION_LEAVE:
                        rd_kafka_cgrp_partition_del(rkcg, rktp);
                        break;

                case RD_KAFKA_OP_FETCH_STOP|RD_KAFKA_OP_REPLY:
                        /* Reply from toppar FETCH_STOP */
                        rd_kafka_assert(rkcg->rkcg_rk,
                                        rkcg->rkcg_wait_unassign_cnt > 0);
                        rkcg->rkcg_wait_unassign_cnt--;

                        rd_kafka_assert(rkcg->rkcg_rk, rktp->rktp_assigned);
			rd_kafka_assert(rkcg->rkcg_rk,
					rkcg->rkcg_assigned_cnt > 0);
                        rktp->rktp_assigned = 0;
			rkcg->rkcg_assigned_cnt--;

                        /* All unassigned toppars now stopped and commit done:
                         * transition to the next state. */
                        if (rkcg->rkcg_join_state ==
                            RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN)
                                rd_kafka_cgrp_check_unassign_done(rkcg,
                                        "FETCH_STOP done");
                        break;

                case RD_KAFKA_OP_OFFSET_COMMIT:
                        /* Trigger offsets commit. */
                        rd_kafka_cgrp_offsets_commit(rkcg, rko);
                        rko = NULL; /* rko now owned by request */
                        break;

                case RD_KAFKA_OP_COORD_QUERY:
                        rd_kafka_cgrp_coord_query(rkcg,
                                                  rko->rko_err ?
                                                  rd_kafka_err2str(rko->
                                                                   rko_err):
                                                  "from op");
                        break;

                case RD_KAFKA_OP_SUBSCRIBE:
                        /* New atomic subscription (may be NULL) */
                        err = rd_kafka_cgrp_subscribe(
                                rkcg, rko->rko_u.subscribe.topics);
                        if (!err)
                                rko->rko_u.subscribe.topics = NULL; /* list owned by rkcg */
                        rd_kafka_op_reply(rko, err);
			rko = NULL;
                        break;

                case RD_KAFKA_OP_ASSIGN:
                        /* New atomic assignment (payload != NULL),
			 * or unassignment (payload == NULL) */
			if (rko->rko_u.assign.partitions &&
			    rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE) {
				/* Dont allow new assignments when terminating */
				err = RD_KAFKA_RESP_ERR__DESTROY;
			} else {
				rd_kafka_cgrp_assign(
					rkcg, rko->rko_u.assign.partitions);
				err = 0;
			}
                        rd_kafka_op_reply(rko, err);
			rko = NULL;
                        break;

                case RD_KAFKA_OP_GET_SUBSCRIPTION:
                        if (rkcg->rkcg_subscription)
				rko->rko_u.subscribe.topics =
					rd_kafka_topic_partition_list_copy(
						rkcg->rkcg_subscription);
                        rd_kafka_op_reply(rko, 0);
			rko = NULL;
			break;

                case RD_KAFKA_OP_GET_ASSIGNMENT:
                        if (rkcg->rkcg_assignment)
				rko->rko_u.assign.partitions =
					rd_kafka_topic_partition_list_copy(
						rkcg->rkcg_assignment);

                        rd_kafka_op_reply(rko, 0);
			rko = NULL;
			break;

                case RD_KAFKA_OP_TERMINATE:
                        rd_kafka_cgrp_terminate0(rkcg, rko);
                        rko = NULL; /* terminate0() takes ownership */
                        break;

                default:
			if (!rd_kafka_op_handle_std(rkcg->rkcg_rk, rko))
				rd_kafka_assert(rkcg->rkcg_rk, !*"unknown type");
                        break;
                }

                if (rko)
                        rd_kafka_op_destroy(rko);
        }
}


/**
 * Client group's join state handling
 */
static void rd_kafka_cgrp_join_state_serve (rd_kafka_cgrp_t *rkcg,
                                            rd_kafka_broker_t *rkb) {

        if (0) // FIXME
        rd_rkb_dbg(rkb, CGRP, "JOINFSM",
                   "Group \"%s\" in join state %s with%s subscription",
                   rkcg->rkcg_group_id->str,
                   rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                   rkcg->rkcg_subscription ? "" : "out");

        switch (rkcg->rkcg_join_state)
        {
        case RD_KAFKA_CGRP_JOIN_STATE_INIT:
                /* If we have a subscription start the join process. */
                if (!rkcg->rkcg_subscription)
                        break;

                if (rd_interval_immediate(&rkcg->rkcg_join_intvl,
					  1000*1000, 0) > 0)
                        rd_kafka_cgrp_join(rkcg);
                break;

        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_JOIN:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC:
        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_UNASSIGN:
	case RD_KAFKA_CGRP_JOIN_STATE_WAIT_REVOKE_REBALANCE_CB:
		break;

        case RD_KAFKA_CGRP_JOIN_STATE_WAIT_ASSIGN_REBALANCE_CB:
        case RD_KAFKA_CGRP_JOIN_STATE_ASSIGNED:
	case RD_KAFKA_CGRP_JOIN_STATE_STARTED:
                if (rkcg->rkcg_flags & RD_KAFKA_CGRP_F_SUBSCRIPTION &&
                    rd_interval(&rkcg->rkcg_heartbeat_intvl,
                                rkcg->rkcg_rk->rk_conf.
                                group_heartbeat_intvl_ms * 1000, 0) > 0)
                        rd_kafka_cgrp_heartbeat(rkcg, rkb);
                break;
        }

}
/**
 * Client group handling.
 * Called from main thread to serve the operational aspects of a cgrp.
 */
void rd_kafka_cgrp_serve (rd_kafka_cgrp_t *rkcg) {
	rd_kafka_broker_t *rkb = rkcg->rkcg_rkb;
	int rkb_state = RD_KAFKA_BROKER_STATE_INIT;
        rd_ts_t now;

	if (rkb) {
		rd_kafka_broker_lock(rkb);
		rkb_state = rkb->rkb_state;
		rd_kafka_broker_unlock(rkb);

		/* Go back to querying state if we lost the current coordinator
		 * connection. */
		if (rkb_state < RD_KAFKA_BROKER_STATE_UP &&
		    rkcg->rkcg_state == RD_KAFKA_CGRP_STATE_UP)
			rd_kafka_cgrp_set_state(rkcg,
						RD_KAFKA_CGRP_STATE_QUERY_COORD);
	}

        rd_kafka_cgrp_op_serve(rkcg, rkb);

        now = rd_clock();

	/* Check for cgrp termination */
	if (unlikely(rd_kafka_cgrp_try_terminate(rkcg))) {
                rd_kafka_cgrp_terminated(rkcg);
                return; /* cgrp terminated */
        }

        /* Bail out if we're terminating. */
        if (unlikely(rd_kafka_terminating(rkcg->rkcg_rk)))
                return;

        switch (rkcg->rkcg_state)
        {
        case RD_KAFKA_CGRP_STATE_TERM:
                break;

        case RD_KAFKA_CGRP_STATE_INIT:
                rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_QUERY_COORD);
                /* FALLTHRU */

        case RD_KAFKA_CGRP_STATE_QUERY_COORD:
                /* Query for coordinator. */
                if (rd_interval_immediate(&rkcg->rkcg_coord_query_intvl,
					  500*1000, now) > 0)
                        rd_kafka_cgrp_coord_query(rkcg,
                                                  "intervaled in "
                                                  "state query-coord");
                break;

        case RD_KAFKA_CGRP_STATE_WAIT_COORD:
                /* Waiting for GroupCoordinator response */
                break;

        case RD_KAFKA_CGRP_STATE_WAIT_BROKER:
                /* See if the group should be reassigned to another broker. */
                if (rd_kafka_cgrp_reassign_broker(rkcg))
                        break;

                /* Coordinator query */
                if (rd_interval(&rkcg->rkcg_coord_query_intvl,
				1000*1000, now) > 0)
                        rd_kafka_cgrp_coord_query(rkcg,
                                                  "intervaled in "
                                                  "state wait-broker");
                break;

        case RD_KAFKA_CGRP_STATE_WAIT_BROKER_TRANSPORT:
                /* Waiting for broker transport to come up.
		 * Also make sure broker supports groups. */
                if (rkb_state < RD_KAFKA_BROKER_STATE_UP || !rkb ||
		    !rd_kafka_broker_supports(
			    rkb, RD_KAFKA_FEATURE_BROKER_GROUP_COORD)) {
			/* Coordinator query */
			if (rd_interval(&rkcg->rkcg_coord_query_intvl,
					1000*1000, now) > 0)
				rd_kafka_cgrp_coord_query(
					rkcg,
					"intervaled in state "
					"wait-broker-transport");

                } else {
                        rd_kafka_cgrp_set_state(rkcg, RD_KAFKA_CGRP_STATE_UP);

                        /* Start fetching if we have an assignment. */
                        if (rkcg->rkcg_assignment &&
			    RD_KAFKA_CGRP_CAN_FETCH_START(rkcg))
                                rd_kafka_cgrp_partitions_fetch_start(
                                        rkcg, rkcg->rkcg_assignment, 0);
                }
                break;

        case RD_KAFKA_CGRP_STATE_UP:
		/* Move any ops awaiting the coordinator to the ops queue
		 * for reprocessing. */
		rd_kafka_q_concat(rkcg->rkcg_ops, rkcg->rkcg_wait_coord_q);

                /* Relaxed coordinator queries. */
                if (rd_interval(&rkcg->rkcg_coord_query_intvl,
                                rkcg->rkcg_rk->rk_conf.
                                coord_query_intvl_ms * 1000, now) > 0)
                        rd_kafka_cgrp_coord_query(rkcg,
                                                  "intervaled in state up");

		if (rkb &&
		    rd_kafka_broker_supports(
			    rkb, RD_KAFKA_FEATURE_BROKER_BALANCED_CONSUMER))
			rd_kafka_cgrp_join_state_serve(rkcg, rkb);
                break;

        }

        if (unlikely(rkcg->rkcg_state != RD_KAFKA_CGRP_STATE_UP &&
                     rd_interval(&rkcg->rkcg_timeout_scan_intvl,
                                 1000*1000, now) > 0))
                rd_kafka_cgrp_timeout_scan(rkcg, now);
}





/**
 * Send an op to a cgrp.
 *
 * Locality: any thread
 */
void rd_kafka_cgrp_op (rd_kafka_cgrp_t *rkcg, rd_kafka_toppar_t *rktp,
                       rd_kafka_replyq_t replyq, rd_kafka_op_type_t type,
                       rd_kafka_resp_err_t err) {
        rd_kafka_op_t *rko;

        rko = rd_kafka_op_new(type);
        rko->rko_err = err;
	rko->rko_replyq = replyq;

	if (rktp)
                rko->rko_rktp = rd_kafka_toppar_keep(rktp);

        rd_kafka_q_enq(rkcg->rkcg_ops, rko);
}







void rd_kafka_cgrp_set_member_id (rd_kafka_cgrp_t *rkcg, const char *member_id){
        if (rkcg->rkcg_member_id && member_id &&
            !rd_kafkap_str_cmp_str(rkcg->rkcg_member_id, member_id))
                return; /* No change */

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "MEMBERID",
                     "Group \"%.*s\": updating member id \"%s\" -> \"%s\"",
                     RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                     rkcg->rkcg_member_id ?
                     rkcg->rkcg_member_id->str : "(not-set)",
                     member_id ? member_id : "(not-set)");

        if (rkcg->rkcg_member_id) {
                rd_kafkap_str_destroy(rkcg->rkcg_member_id);
                rkcg->rkcg_member_id = NULL;
        }

        if (member_id)
                rkcg->rkcg_member_id = rd_kafkap_str_new(member_id, -1);
}



static void
rd_kafka_cgrp_assignor_run (rd_kafka_cgrp_t *rkcg,
                            const char *protocol_name,
			    rd_kafka_resp_err_t err,
                            rd_kafka_metadata_t *metadata,
                            rd_kafka_group_member_t *members,
                            int member_cnt) {
        char errstr[512];

	if (err) {
		rd_snprintf(errstr, sizeof(errstr),
			    "Failed to get cluster metadata: %s",
			    rd_kafka_err2str(err));
		goto err;
	}

	*errstr = '\0';

	/* Run assignor */
	err = rd_kafka_assignor_run(rkcg, protocol_name, metadata,
				    members, member_cnt,
				    errstr, sizeof(errstr));

	if (err) {
		if (!*errstr)
			rd_snprintf(errstr, sizeof(errstr), "%s",
				    rd_kafka_err2str(err));
		goto err;
	}

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "ASSIGNOR",
                     "Group \"%s\": \"%s\" assignor run for %d member(s)",
                     rkcg->rkcg_group_id->str, protocol_name, member_cnt);

	rd_kafka_cgrp_set_join_state(rkcg, RD_KAFKA_CGRP_JOIN_STATE_WAIT_SYNC);

        /* Respond to broker with assignment set or error */
        rd_kafka_SyncGroupRequest(rkcg->rkcg_rkb,
                                  rkcg->rkcg_group_id, rkcg->rkcg_generation_id,
                                  rkcg->rkcg_member_id,
                                  members, err ? 0 : member_cnt,
                                  RD_KAFKA_REPLYQ(rkcg->rkcg_ops, 0),
                                  rd_kafka_handle_SyncGroup, rkcg);
        return;

err:
        rd_kafka_log(rkcg->rkcg_rk, LOG_ERR, "ASSIGNOR",
                     "Group \"%s\": failed to run assignor \"%s\" for "
                     "%d member(s): %s",
                     rkcg->rkcg_group_id->str, protocol_name,
                     member_cnt, errstr);

        rd_kafka_cgrp_set_join_state(rkcg, RD_KAFKA_CGRP_JOIN_STATE_INIT);

}


void rd_kafka_cgrp_handle_Metadata (rd_kafka_cgrp_t *rkcg,
                                    rd_kafka_resp_err_t err,
                                    rd_kafka_metadata_t *md) {

        if (rkcg->rkcg_join_state != RD_KAFKA_CGRP_JOIN_STATE_WAIT_METADATA)
                return;

        rd_kafka_cgrp_assignor_run(rkcg,
                                   rkcg->rkcg_group_leader.protocol,
                                   err, md,
                                   rkcg->rkcg_group_leader.members,
                                   rkcg->rkcg_group_leader.member_cnt);
}

/**
 * Check if the latest metadata affects the current subscription:
 * - matched topic added
 * - matched topic removed
 * - matched topic's partition count change
 */
void rd_kafka_cgrp_metadata_update_check (rd_kafka_cgrp_t *rkcg,
					  const struct rd_kafka_metadata *md) {
	rd_list_t *topics;

	rd_kafka_assert(NULL, thrd_is_current(rkcg->rkcg_rk->rk_thread));

	if (!rkcg->rkcg_subscription || rkcg->rkcg_subscription->cnt == 0)
		return;

	/*
	 * Create a list of the topics in metadata that matches our subscription
	 */
	topics = rd_list_new(rkcg->rkcg_subscription->cnt);

	rd_kafka_metadata_topic_match(rkcg->rkcg_rk,
				      topics, md, rkcg->rkcg_subscription);


	/*
	 * Update
	 */
	if (rd_kafka_cgrp_update_subscribed_topics(rkcg, topics)) {
		/* List of subscribed topics changed, trigger rejoin. */
		rd_kafka_cgrp_rejoin(rkcg);
	}
}


void rd_kafka_cgrp_handle_SyncGroup (rd_kafka_cgrp_t *rkcg,
				     rd_kafka_broker_t *rkb,
                                     rd_kafka_resp_err_t err,
                                     const rd_kafkap_bytes_t *member_state) {
        rd_kafka_buf_t *rkbuf = NULL;
        rd_kafka_topic_partition_list_t *assignment;
        const int log_decode_errors = 1;
        int16_t Version;
        int32_t TopicCnt;
        rd_kafkap_bytes_t UserData;
        rd_kafka_group_member_t rkgm;

	/* Dont handle new assignments when terminating */
	if (!err && rkcg->rkcg_flags & RD_KAFKA_CGRP_F_TERMINATE)
		err = RD_KAFKA_RESP_ERR__DESTROY;

        if (err)
                goto err;


	if (RD_KAFKAP_BYTES_LEN(member_state) == 0) {
		/* Empty assignment. */
		assignment = rd_kafka_topic_partition_list_new(0);
		memset(&UserData, 0, sizeof(UserData));
		goto done;
	}

        /* Parse assignment from MemberState */
        rkbuf = rd_kafka_buf_new_shadow(member_state->data,
                                        RD_KAFKAP_BYTES_LEN(member_state));
	/* Protocol parser needs a broker handle to log errors on. */
	if (rkb) {
		rkbuf->rkbuf_rkb = rkb;
		rd_kafka_broker_keep(rkb);
	} else
		rkbuf->rkbuf_rkb = rd_kafka_broker_internal(rkcg->rkcg_rk);

        rd_kafka_buf_read_i16(rkbuf, &Version);
        rd_kafka_buf_read_i32(rkbuf, &TopicCnt);

        if (TopicCnt > 10000) {
                err = RD_KAFKA_RESP_ERR__BAD_MSG;
                goto err;
        }

        assignment = rd_kafka_topic_partition_list_new(TopicCnt);
        while (TopicCnt-- > 0) {
                rd_kafkap_str_t Topic;
                int32_t PartCnt;
                rd_kafka_buf_read_str(rkbuf, &Topic);
                rd_kafka_buf_read_i32(rkbuf, &PartCnt);
                while (PartCnt-- > 0) {
                        int32_t Partition;
			char *topic_name;
			RD_KAFKAP_STR_DUPA(&topic_name, &Topic);
                        rd_kafka_buf_read_i32(rkbuf, &Partition);

                        rd_kafka_topic_partition_list_add(
                                assignment, topic_name, Partition);
                }
        }

        rd_kafka_buf_read_bytes(rkbuf, &UserData);
        rkbuf->rkbuf_buf2 = NULL; /* Avoid free of underlying memory */
        rd_kafka_buf_destroy(rkbuf);

 done:
        memset(&rkgm, 0, sizeof(rkgm));
        rkgm.rkgm_assignment = assignment;
        rkgm.rkgm_userdata = &UserData;

        /* Set the new assignment */
	rd_kafka_cgrp_handle_assignment(rkcg, assignment);

        rd_kafka_topic_partition_list_destroy(assignment);

        return;
err:
        if (rkbuf) {
                rkbuf->rkbuf_buf2 = NULL; /* Avoid free of underlying memory */
                rd_kafka_buf_destroy(rkbuf);
        }

        rd_kafka_dbg(rkcg->rkcg_rk, CGRP, "GRPSYNC",
                     "Group \"%s\": synchronization failed: %s: rejoining",
                     rkcg->rkcg_group_id->str, rd_kafka_err2str(err));
        rd_kafka_cgrp_set_join_state(rkcg, RD_KAFKA_CGRP_JOIN_STATE_INIT);
}
