/* -*- C -*- */
/*
 * Copyright (c) 2021 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


#pragma once

#ifndef __MOTR_DTM0_RECOVERY_H__
#define __MOTR_DTM0_RECOVERY_H__

/* DTM0 basic recovery DLD.
 *
 * Definition
 * ----------
 *
 *   DTM0 basic recovery defines a set of states and transitions between them
 * for each participant of the cluster considering only a limited set of
 * usecases.
 *
 *
 * Assumptions (limitations)
 * ---------------------------
 *
 *  A1: HA (Cortx-Hare or Cortx-Hare+Cortx-HA) provides EOS for each event.
 *  A2: HA provides total ordering for all events in the system.
 *  *A3: DTM0 handles HA events one-by-one without filtering.
 *  A4: DTM0 user relies on EXECUTED and STABLE events to ensure proper ordering
 *      of DTXes.
 *  A5: There may be a point on the timeline where data is not fully replicated
 *      across the cluster.
 *  A6: RPC connections to a FAILED/TRANSIENT participant are terminated as soon
 *      as HA sends the corresponding event.
 *  A7: HA epochs are not exposed to DTM0 even if HA replies on them.
 *  A8: Recovery is performed by participants with persistent state.
 *
 *
 * HA states and HA events
 * -----------------------
 *
 *   Every participant (PA) has the following set of HA states:
 *     TRANSIENT
 *     ONLINE
 *     RECOVERING
 *     FAILED
 * States are assigned by HA: it delivers an HA event to every participant
 * whenever it decided that one of the participants has changed its state.
 *   Motr-HA subsystem provides the "receiver-side" guarantee to comply with A1.
 * It has its own log of events that is being updated whenever an HA event
 * has been fully handled.
 *
 *
 * State transitions (HA side)
 * ---------------------------
 *
 * PROCESS_STARTING: TRANSIENT -> RECOVERING
 *   HA sends "Recovering" when the corresponding
 *   participant sends PROCESS_STARTED event to HA.
 *
 * PROCESS_STARTED: RECOVERING -> ONLINE
 *   When the process finished recovering from a failure, it sends
 *   PROCESS_STARTED to HA. HA notifies everyone that this process is back
 *   ONLINE.
 *
 * <PROCESS_T_FAILED>: ONLINE -> TRANSIENT
 *   When HA detects a transient failure, it notifies everyone about it.
 *
 * <PROCESS_FAILED>: [ONLINE, RECOVERING, TRANSIENT] -> FAILED
 *   Whenever HA detects a permanent failure, it starts eviction of the
 *   corresponding participant from the cluster.
 *
 *
 * HA Events (Motr side)
 * ---------------------
 *
 *   PROCESS_STARTING is sent when Motr process is ready to start recovering
 * procedures, and it is ready to serve WRITE-like requests.
 *   PROCESS_STARTED is sent when Motr process finished its recovery procedures,
 * and it is ready to serve READ-like requests.
 *
 *
 * State transitions (DTM0 side)
 * -----------------------------
 *
 *  TRANSIENT -> RECOVERING: DTM0 service launches a recovery FOM. There are
 *    two types of recovery FOMs: local recovery FOM and remote recovery FOM.
 *    The type is based on the participant that has changed its state.
 *  RECOVERING -> ONLINE: DTM0 service stops/finalises recovery FOMs for the
 *    participant that has changed its state.
 *  ONLINE -> TRANSIENT: DTM0 service terminates DTM0 RPC links established
 *    to the participant that has changed its state (see A6).
 *  [ONLINE, RECOVERING, TRANSIENT] -> FAILED: DTM0 service launches a recovery
 *    FOM to handle evition (remote eviction FOM).
 *
 *
 * DTM0 recovery FOMs
 * ------------------
 *
 *   Local recovery FOM. A local recovery FOM is supposed to handle recovery
 * of the process where the fom has been launched. When the recovery stop
 * condition is satisfied, the FOM causes PROCESS_STARTED event.
 *   Remote recovery FOM. This FOM uses the local DTM0 log to replay DTM0
 * transactions to a remote participant.
 *   Remote eviction FOM. The FOM is similar to the remote recovery FOM but
 * it replays the logs to every participant.
 *
 * DTM0 recovery stop condition
 * ----------------------------
 *   The stop condition is used to stop recovery FOMs and propagate state
 * transitions to HA. Recovery stops when a participant "cathes up" with
 * the new cluster state. Since we allow WRITE-like operations to be performed
 * at almost any time (the exceptions are TRANSIENT and FAILED states of
 * of the target participants), the stop condition relies on the information
 * about the difference between the DTM0 log state when recovery started (1)
 * and the point where recovery ended (2). The second point is defined using
 * so-called "recovery stop condition". Each recovery FOM has its own condition
 * described in the next sections.
 *   Note, HA epochs would let us define a quite precise boundaries for the
 * stop. Howver, as it is stated in A7, we cannot reply on them. Because of
 * that, we simply store the first WRITE-like operation on the participant
 * experiencing RECOVERING, so that this participant can make a decision
 * whether recovery has to be stopped or not.
 *
 *
 * Stop condition for local FOM
 * ----------------------------
 *
 *   The local recovery FOM is created before the local process sends
 * PROCESS_STARTED event the HA. Then, it stores (in volatile memory) the first
 * WRITE-like operation that arrived after the FOM was created. Since clients
 * cannot send WRITE-like operations to servers in TRANSIENT or FAILED state,
 * and leaving of such a state happens only a local recovery FOM is created,
 * it helps to avoid a race window, as it is shown below:
 *
 * @verbatim
 *   P1.HA     | P1.TRANSIENT        | P1.RECOVERING
 *   P1.Motr   | <down> | <starting> |
 *   P1.DTM    | <down>                          | <started recovery FOM>
 *   C1.HA         | P1.TRANSIENT          | P1.RECOVERING
 *   C1.Motr   | <ongoing IO to other PAs>  | <sends KVS PUT to P1>
 *             -------------------------------------------------> real time
 *                                   1    2 3    4
 *
 *   (1) HA on P1 learns about the state transition.
 *   (2) HA on P2 learns about the state transition.
 *   (3) Client sends KVS PUT (let's say it gets delivered almost instantly).
 *   (4) A recovery FOM was launched on P1. It missed the KVS PUT.
 * @endverbatim
 *
 *   The stop condition for a local recovery FOM can be broken down to the
 * following sub-conditions applied to every ONLINE-or-TRANSIENT participant
 * of the cluster:
 *   1. Caught-up: participant sends a log record that has the same TX ID as
 *   the first operation recorded by the local recovery FOM.
 *   2. End of log: participant send "end-of-the-log" message.
 * In other words, a participant leaves RECOVERING state when it got REDOs
 * from all the others until the point where new IO was started. Note, there
 * are a lot of corner cases related to interleaving and clock synchronisation
 * but they are not covered here.
 *
 *
 * Stop condition for remote FOM
 * -----------------------------
 *
 *   The next question is "when a remote process should stop sending REDOs?".
 * This question defines the stop condition for a remote recovering FOM. Since
 * there is a race window between the point where a remote process gets
 * RECOVERING event and the point where one of the clients sends a "new"
 * WRITE-like operations, remote participants are not able to use the
 * "until first WRITE" condition. Instead, they rely on the responses from the
 * participant that is being recovered.
 *
 * Stop condition for eviction FOM
 * -------------------------------
 *
 *   The final question is how to stop eviction of a FAILED participant. The
 * stop condition here is that all the records where the participant
 * participated should be replayed to the other participants:
 *
 *
 * REDO messages
 * -------------
 *
 *   REDO messages contain full updates. They are sent one-by-one by a
 * recovering FOM (local/remote/eviction). A REDO message is populated using
 * a DTM0 log record. A user service (CAS, IOS) provides a callback to DTM0
 * service that transmutes a serialised request sent from the client to a
 * FOP that can be sent from one server to another.
 *   A log record can be modified in a conflict-like manner only by the local
 * log prunner daemon. DTM0 log locks or the locality lock can be used to avoid
 * such kind of conflicts (up to the point where a shot-live lock can be taken
 * to make a copy of a log record).
 *   The receiver of REDO message should send back a DTM0 PERSISTENT message or
 * an array of such messages. The requester applies the Pmsgs from the reply
 * to its own log and then send the next REDO message.
 *   When the sender of REDO message reaches the end of its journal, it should
 * send a special message that would indicate that the last REDO message was
 * the final message. A special field is added to REDO message to achieve that:
 * @verbatim
 *   REDO message {
 *     tx_desc -- transaction descriptor (partial update)
 *     payload -- transmuted original request
 *     is_last -- a boolean flag
 *   }
 * @endverbatim
 */

#endif /* __MOTR_DTM0_RECOVERY_H__ */

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
/*
 * vim: tabstop=8 shiftwidth=8 noexpandtab textwidth=80 nowrap
 */
