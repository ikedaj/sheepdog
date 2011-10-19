/*
 * Copyright (C) 2009-2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/epoll.h>

#include "sheepdog_proto.h"
#include "sheep_priv.h"
#include "list.h"
#include "util.h"
#include "logger.h"
#include "work.h"
#include "cluster.h"

struct node {
	struct sheepid sheepid;
	struct sheepdog_node_list_entry ent;
	struct list_head list;
};

enum deliver_msg_state {
	DM_INIT = 1,
	DM_CONT,
	DM_FIN,
};

struct message_header {
	uint8_t proto_ver;
	uint8_t pad;
	uint8_t op;
	uint8_t state;
	uint32_t msg_length;
	struct sheepid sheepid;
	struct sheepdog_node_list_entry from;
};

struct join_message {
	struct message_header header;
	uint32_t nr_nodes;
	uint32_t nr_sobjs;
	uint32_t cluster_status;
	uint32_t epoch;
	uint64_t ctime;
	uint32_t result;
	uint8_t inc_epoch; /* set non-zero when we increment epoch of all nodes */
	uint8_t pad[3];
	struct {
		struct sheepid sheepid;
		struct sheepdog_node_list_entry ent;
	} nodes[SD_MAX_NODES];
	uint32_t nr_leave_nodes;
	struct {
		struct sheepid sheepid;
		struct sheepdog_node_list_entry ent;
	} leave_nodes[SD_MAX_NODES];
};

struct leave_message {
	struct message_header header;
	uint32_t epoch;
};

struct vdi_op_message {
	struct message_header header;
	struct sd_vdi_req req;
	struct sd_vdi_rsp rsp;
	uint8_t data[0];
};

struct mastership_tx_message {
	struct message_header header;
	uint32_t epoch;
};

struct work_notify {
	struct cpg_event cev;

	struct message_header *msg;
};

struct work_join {
	struct cpg_event cev;

	struct sheepid *member_list;
	size_t member_list_entries;
	struct sheepid joined;
};

struct work_leave {
	struct cpg_event cev;

	struct sheepid *member_list;
	size_t member_list_entries;
	struct sheepid left;
};

#define print_node_list(node_list)				\
({								\
	struct node *__node;					\
	char __name[128];					\
	list_for_each_entry(__node, node_list, list) {		\
		dprintf("%c pid: %ld, ip: %s\n",		\
			is_myself(__node->ent.addr, __node->ent.port) ? 'l' : ' ',	\
			__node->sheepid.pid,			\
			addr_to_str(__name, sizeof(__name),	\
			__node->ent.addr, __node->ent.port));	\
	}							\
})

enum cpg_event_work_bits {
	CPG_EVENT_WORK_RUNNING = 1,
	CPG_EVENT_WORK_SUSPENDED,
	CPG_EVENT_WORK_JOINING,
};

#define CPG_EVENT_WORK_FNS(bit, name)					\
static int cpg_event_##name(void)					\
{									\
	return test_bit(CPG_EVENT_WORK_##bit,				\
		&sys->cpg_event_work_flags);				\
}									\
static void cpg_event_clear_##name(void)				\
{									\
	clear_bit(CPG_EVENT_WORK_##bit, &sys->cpg_event_work_flags);	\
}									\
static void cpg_event_set_##name(void)					\
{									\
	set_bit(CPG_EVENT_WORK_##bit, &sys->cpg_event_work_flags);	\
}

CPG_EVENT_WORK_FNS(RUNNING, running)
CPG_EVENT_WORK_FNS(SUSPENDED, suspended)
CPG_EVENT_WORK_FNS(JOINING, joining)

static inline int join_message(struct message_header *m)
{
	return m->op == SD_MSG_JOIN;
}

static inline int vdi_op_message(struct message_header *m)
{
	return m->op == SD_MSG_VDI_OP;
}

static inline int master_chg_message(struct message_header *m)
{
	return m->op == SD_MSG_MASTER_CHANGED;
}

static inline int leave_message(struct message_header *m)
{
	return m->op == SD_MSG_LEAVE;
}

static inline int master_tx_message(struct message_header *m)
{
	return m->op == SD_MSG_MASTER_TRANSFER;
}

static int get_node_idx(struct sheepdog_node_list_entry *ent,
			struct sheepdog_node_list_entry *entries, int nr_nodes)
{
	ent = bsearch(ent, entries, nr_nodes, sizeof(*ent), node_cmp);
	if (!ent)
		return -1;

	return ent - entries;
}

static void build_node_list(struct list_head *node_list,
			    struct sheepdog_node_list_entry *entries,
			    int *nr_nodes, int *nr_zones)
{
	struct node *node;
	int nr = 0, i;
	uint32_t zones[SD_MAX_REDUNDANCY];

	if (nr_zones)
		*nr_zones = 0;

	list_for_each_entry(node, node_list, list) {
		if (entries)
			memcpy(entries + nr, &node->ent, sizeof(*entries));
		nr++;

		if (nr_zones && *nr_zones < ARRAY_SIZE(zones)) {
			for (i = 0; i < *nr_zones; i++) {
				if (zones[i] == node->ent.zone)
					break;
			}
			if (i == *nr_zones)
				zones[(*nr_zones)++] = node->ent.zone;
		}
	}
	if (entries)
		qsort(entries, nr, sizeof(*entries), node_cmp);
	if (nr_nodes)
		*nr_nodes = nr;
}

int get_ordered_sd_node_list(struct sheepdog_node_list_entry *entries)
{
	int nr_nodes;

	build_node_list(&sys->sd_node_list, entries, &nr_nodes, NULL);

	return nr_nodes;
}

void get_ordered_sd_vnode_list(struct sheepdog_vnode_list_entry *entries,
			       int *nr_vnodes, int *nr_zones)
{
	struct sheepdog_node_list_entry nodes[SD_MAX_NODES];
	int nr;

	build_node_list(&sys->sd_node_list, nodes, &nr, nr_zones);

	if (sys->nr_vnodes == 0)
		sys->nr_vnodes = nodes_to_vnodes(nodes, nr, sys->vnodes);

	memcpy(entries, sys->vnodes, sizeof(*entries) * sys->nr_vnodes);

	*nr_vnodes = sys->nr_vnodes;
}

void setup_ordered_sd_vnode_list(struct request *req)
{
	get_ordered_sd_vnode_list(req->entry, &req->nr_vnodes, &req->nr_zones);
}

static void get_node_list(struct sd_node_req *req,
			  struct sd_node_rsp *rsp, void *data)
{
	int nr_nodes;
	struct node *node;

	nr_nodes = get_ordered_sd_node_list(data);
	rsp->data_length = nr_nodes * sizeof(struct sheepdog_node_list_entry);
	rsp->nr_nodes = nr_nodes;
	rsp->local_idx = get_node_idx(&sys->this_node, data, nr_nodes);

	if (!nr_nodes) {
		rsp->master_idx = -1;
		return;
	}
	node = list_first_entry(&sys->sd_node_list, struct node, list);
	rsp->master_idx = get_node_idx(&node->ent, data, nr_nodes);
}

static int get_epoch(struct sd_obj_req *req,
		      struct sd_obj_rsp *rsp, void *data)
{
	int epoch = req->tgt_epoch;
	int len, ret;
	dprintf("%d\n", epoch);
	len = epoch_log_read(epoch, (char *)data, req->data_length);
	if (len == -1) {
		ret = SD_RES_NO_TAG;
		rsp->data_length = 0;
	} else {
		ret = SD_RES_SUCCESS;
		rsp->data_length = len;
	}
	return ret;
}

void cluster_queue_request(struct work *work, int idx)
{
	struct request *req = container_of(work, struct request, work);
	struct sd_req *hdr = (struct sd_req *)&req->rq;
	struct sd_rsp *rsp = (struct sd_rsp *)&req->rp;
	struct vdi_op_message *msg;
	struct epoch_log *log;
	int ret = SD_RES_SUCCESS, i, max_logs, epoch;

	eprintf("%p %x\n", req, hdr->opcode);

	switch (hdr->opcode) {
	case SD_OP_GET_EPOCH:
		ret = get_epoch((struct sd_obj_req *)hdr,
			  (struct sd_obj_rsp *)rsp, req->data);
		break;
	case SD_OP_GET_NODE_LIST:
		get_node_list((struct sd_node_req *)hdr,
			      (struct sd_node_rsp *)rsp, req->data);
		break;
	case SD_OP_STAT_CLUSTER:
		max_logs = rsp->data_length / sizeof(*log);
		epoch = get_latest_epoch();
		rsp->data_length = 0;
		for (i = 0; i < max_logs; i++) {
			if (epoch <= 0)
				break;

			log = (struct epoch_log *)req->data + i;
			log->epoch = epoch;
			log->ctime = get_cluster_ctime();
			log->nr_nodes = epoch_log_read(epoch, (char *)log->nodes,
						       sizeof(log->nodes));
			if (log->nr_nodes == -1)
				log->nr_nodes = epoch_log_read_remote(epoch,
								      (char *)log->nodes,
								      sizeof(log->nodes));

			rsp->data_length += sizeof(*log);
			log->nr_nodes /= sizeof(log->nodes[0]);
			epoch--;
		}

		switch (sys->status) {
		case SD_STATUS_OK:
			ret = SD_RES_SUCCESS;
			break;
		case SD_STATUS_WAIT_FOR_FORMAT:
			ret = SD_RES_WAIT_FOR_FORMAT;
			break;
		case SD_STATUS_WAIT_FOR_JOIN:
			ret = SD_RES_WAIT_FOR_JOIN;
			break;
		case SD_STATUS_SHUTDOWN:
			ret = SD_RES_SHUTDOWN;
			break;
		case SD_STATUS_JOIN_FAILED:
			ret = SD_RES_JOIN_FAILED;
			break;
		case SD_STATUS_HALT:
			ret = SD_RES_HALT;
			break;
		default:
			ret = SD_RES_SYSTEM_ERROR;
			break;
		}
		break;
	default:
		/* forward request to group */
		goto forward;
	}

	rsp->result = ret;
	return;

forward:
	msg = zalloc(sizeof(*msg) + hdr->data_length);
	if (!msg) {
		eprintf("out of memory\n");
		return;
	}

	msg->header.op = SD_MSG_VDI_OP;
	msg->header.state = DM_INIT;
	msg->header.msg_length = sizeof(*msg) + hdr->data_length;
	msg->header.from = sys->this_node;
	msg->req = *((struct sd_vdi_req *)&req->rq);
	msg->rsp = *((struct sd_vdi_rsp *)&req->rp);
	if (hdr->flags & SD_FLAG_CMD_WRITE)
		memcpy(msg->data, req->data, hdr->data_length);

	list_add(&req->pending_list, &sys->pending_list);

	sys->cdrv->notify(msg, msg->header.msg_length);

	free(msg);
}

static void group_handler(int listen_fd, int events, void *data)
{
	int ret;
	if (events & EPOLLHUP) {
		eprintf("Receive EPOLLHUP event. Is corosync stopped running?\n");
		goto out;
	}

	ret = sys->cdrv->dispatch();
	if (ret == 0)
		return;
	else
		eprintf("oops...some error occured inside corosync\n");
out:
	log_close();
	exit(1);
}

static struct node *find_node(struct list_head *node_list, struct sheepid *id)
{
	struct node *node;

	list_for_each_entry(node, node_list, list) {
		if (sheepid_cmp(&node->sheepid, id) == 0)
			return node;
	}

	return NULL;
}

static int is_master(void)
{
	struct node *node;

	if (!sys->join_finished)
		return 0;

	node = list_first_entry(&sys->sd_node_list, struct node, list);
	if (is_myself(node->ent.addr, node->ent.port))
		return 1;
	return 0;
}

static inline int get_nodes_nr_from(struct list_head *l)
{
	struct node *node;
	int nr = 0;
	list_for_each_entry(node, l, list) {
		nr++;
	}
	return nr;
}

static int get_nodes_nr_epoch(int epoch)
{
	struct sheepdog_node_list_entry nodes[SD_MAX_NODES];
	int nr;

	nr = epoch_log_read(epoch, (char *)nodes, sizeof(nodes));
	nr /= sizeof(nodes[0]);
	return nr;
}

static struct sheepdog_node_list_entry *find_entry_list(struct sheepdog_node_list_entry *entry,
							struct list_head *head)
{
	struct node *n;
	list_for_each_entry(n, head, list)
		if (node_cmp(&n->ent, entry) == 0)
			return entry;

	return NULL;

}

static struct sheepdog_node_list_entry *find_entry_epoch(struct sheepdog_node_list_entry *entry,
							 int epoch)
{
	struct sheepdog_node_list_entry nodes[SD_MAX_NODES];
	int nr, i;

	nr = epoch_log_read_nr(epoch, (char *)nodes, sizeof(nodes));

	for (i = 0; i < nr; i++)
		if (node_cmp(&nodes[i], entry) == 0)
			return entry;

	return NULL;
}

static int add_node_to_leave_list(struct message_header *msg)
{
	int ret = SD_RES_SUCCESS;
	int nr, i, le = get_latest_epoch();
	LIST_HEAD(tmp_list);
	struct node *n, *t;
	struct join_message *jm;

	if (leave_message(msg)) {
		n = zalloc(sizeof(*n));
		if (!n) {
			ret = SD_RES_NO_MEM;
			goto err;
		}

		if (find_entry_list(&msg->from, &sys->leave_list)
		    || !find_entry_epoch(&msg->from, le)) {
			free(n);
			goto ret;
		}

		n->sheepid = msg->sheepid;
		n->ent = msg->from;

		list_add_tail(&n->list, &sys->leave_list);
		goto ret;
	} else if (join_message(msg)) {
		jm = (struct join_message *)msg;
		nr = jm->nr_leave_nodes;
		for (i = 0; i < nr; i++) {
			n = zalloc(sizeof(*n));
			if (!n) {
				ret = SD_RES_NO_MEM;
				goto free;
			}

			if (find_entry_list(&jm->leave_nodes[i].ent, &sys->leave_list)
			    || !find_entry_epoch(&jm->leave_nodes[i].ent, le)) {
				free(n);
				continue;
			}

			n->sheepid = jm->leave_nodes[i].sheepid;
			n->ent = jm->leave_nodes[i].ent;

			list_add_tail(&n->list, &tmp_list);
		}
		list_splice_init(&tmp_list, &sys->leave_list);
		goto ret;
	} else {
		ret = SD_RES_INVALID_PARMS;
		goto err;
	}
free:
	list_for_each_entry_safe(n, t, &tmp_list, list) {
		free(n);
	}
ret:
	dprintf("%d\n", get_nodes_nr_from(&sys->leave_list));
	print_node_list(&sys->leave_list);
err:
	return ret;
}

static int cluster_sanity_check(struct sheepdog_node_list_entry *entries,
			     int nr_entries, uint64_t ctime, uint32_t epoch)
{
	int ret = SD_RES_SUCCESS, nr_local_entries;
	struct sheepdog_node_list_entry local_entries[SD_MAX_NODES];
	uint32_t lepoch;

	if (sys->status == SD_STATUS_WAIT_FOR_FORMAT ||
	    sys->status == SD_STATUS_SHUTDOWN)
		goto out;
	/* When the joinning node is newly created, we need to check nothing. */
	if (nr_entries == 0)
		goto out;

	if (ctime != get_cluster_ctime()) {
		ret = SD_RES_INVALID_CTIME;
		goto out;
	}

	lepoch = get_latest_epoch();
	if (epoch > lepoch) {
		ret = SD_RES_OLD_NODE_VER;
		goto out;
	}

	if (sys->status == SD_STATUS_OK || sys->status == SD_STATUS_HALT)
		goto out;

	if (epoch < lepoch) {
		ret = SD_RES_NEW_NODE_VER;
		goto out;
	}

	nr_local_entries = epoch_log_read_nr(epoch, (char *)local_entries,
			sizeof(local_entries));

	if (nr_entries != nr_local_entries ||
	    memcmp(entries, local_entries, sizeof(entries[0]) * nr_entries) != 0) {
		ret = SD_RES_INVALID_EPOCH;
		goto out;
	}

out:
	return ret;
}

static int get_cluster_status(struct sheepdog_node_list_entry *from,
			      struct sheepdog_node_list_entry *entries,
			      int nr_entries, uint64_t ctime, uint32_t epoch,
			      uint32_t *status, uint8_t *inc_epoch)
{
	int i, ret = SD_RES_SUCCESS;
	int nr, nr_local_entries, nr_leave_entries;
	struct sheepdog_node_list_entry local_entries[SD_MAX_NODES];
	struct node *node;
	char str[256];

	*status = sys->status;
	if (inc_epoch)
		*inc_epoch = 0;

	ret = cluster_sanity_check(entries, nr_entries, ctime, epoch);
	if (ret)
		goto out;

	switch (sys->status) {
	case SD_STATUS_HALT:
	case SD_STATUS_OK:
		if (inc_epoch)
			*inc_epoch = 1;
		break;
	case SD_STATUS_WAIT_FOR_FORMAT:
		if (nr_entries != 0)
			ret = SD_RES_NOT_FORMATTED;
		break;
	case SD_STATUS_WAIT_FOR_JOIN:
		nr = get_nodes_nr_from(&sys->sd_node_list) + 1;
		nr_local_entries = epoch_log_read_nr(epoch, (char *)local_entries,
						  sizeof(local_entries));

		if (nr != nr_local_entries) {
			nr_leave_entries = get_nodes_nr_from(&sys->leave_list);
			if (nr_local_entries == nr + nr_leave_entries) {
				/* Even though some nodes leave, we can make do with it.
				 * Order cluster to do recovery right now.
				 */
				if (inc_epoch)
					*inc_epoch = 1;
				*status = SD_STATUS_OK;
			}
			break;
		}

		for (i = 0; i < nr_local_entries; i++) {
			if (node_cmp(local_entries + i, from) == 0)
				goto next;
			list_for_each_entry(node, &sys->sd_node_list, list) {
				if (node_cmp(local_entries + i, &node->ent) == 0)
					goto next;
			}
			break;
		next:
			;
		}

		*status = SD_STATUS_OK;
		break;
	case SD_STATUS_SHUTDOWN:
		ret = SD_RES_SHUTDOWN;
		break;
	default:
		break;
	}
out:
	if (ret)
		eprintf("%x, %s\n", ret,
			addr_to_str(str, sizeof(str), from->addr, from->port));

	return ret;
}

static void join(struct join_message *msg)
{
	struct node *node;
	struct sheepdog_node_list_entry entry[SD_MAX_NODES];
	int i;

	if (msg->header.proto_ver != SD_SHEEP_PROTO_VER) {
		eprintf("joining node send a wrong version message\n");
		msg->result = SD_RES_VER_MISMATCH;
		return;
	}

	for (i = 0; i < msg->nr_nodes; i++)
		entry[i] = msg->nodes[i].ent;

	msg->result = get_cluster_status(&msg->header.from, entry,
					 msg->nr_nodes, msg->ctime,
					 msg->epoch, &msg->cluster_status,
					 &msg->inc_epoch);
	msg->nr_sobjs = sys->nr_sobjs;
	msg->ctime = get_cluster_ctime();
	msg->nr_nodes = 0;
	list_for_each_entry(node, &sys->sd_node_list, list) {
		msg->nodes[msg->nr_nodes].sheepid = node->sheepid;
		msg->nodes[msg->nr_nodes].ent = node->ent;
		msg->nr_nodes++;
	}
}

static int get_vdi_bitmap_from(struct sheepdog_node_list_entry *node)
{
	struct sd_req hdr;
	struct sd_rsp *rsp = (struct sd_rsp *)&hdr;
	static DECLARE_BITMAP(tmp_vdi_inuse, SD_NR_VDIS);
	int fd, i, ret = SD_RES_SUCCESS;
	unsigned int rlen, wlen;
	char host[128];

	if (is_myself(node->addr, node->port))
		goto out;

	addr_to_str(host, sizeof(host), node->addr, 0);

	fd = connect_to(host, node->port);
	if (fd < 0) {
		vprintf(SDOG_ERR, "can't get the vdi bitmap %s, %m\n", host);
		ret = -SD_RES_EIO;
		goto out;
	}

	vprintf(SDOG_ERR, "get the vdi bitmap from %s\n", host);

	memset(&hdr, 0, sizeof(hdr));
	hdr.opcode = SD_OP_READ_VDIS;
	hdr.epoch = sys->epoch;
	hdr.data_length = sizeof(tmp_vdi_inuse);
	rlen = hdr.data_length;
	wlen = 0;

	ret = exec_req(fd, &hdr, (char *)tmp_vdi_inuse,
			&wlen, &rlen);

	close(fd);

	if (ret || rsp->result != SD_RES_SUCCESS) {
		vprintf(SDOG_ERR, "can't get the vdi bitmap %d %d\n", ret,
				rsp->result);
		goto out;
	}

	for (i = 0; i < ARRAY_SIZE(sys->vdi_inuse); i++)
		sys->vdi_inuse[i] |= tmp_vdi_inuse[i];
out:
	return ret;
}

static void get_vdi_bitmap_from_sd_list(void)
{
	int i, nr_nodes;
	/* fixme: we need this until starting up. */
	struct sheepdog_node_list_entry nodes[SD_MAX_NODES];

	/*
	 * we don't need the proper order but this is the simplest
	 * way.
	 */
	nr_nodes = get_ordered_sd_node_list(nodes);

	for (i = 0; i < nr_nodes; i++)
		get_vdi_bitmap_from(&nodes[i]);
}

static int move_node_to_sd_list(struct sheepid *id,
				struct sheepdog_node_list_entry ent)
{
	struct node *node;

	node = find_node(&sys->cpg_node_list, id);
	if (!node)
		return 1;

	node->ent = ent;

	list_del(&node->list);
	list_add_tail(&node->list, &sys->sd_node_list);
	sys->nr_vnodes = 0;

	return 0;
}

static int update_epoch_log(int epoch)
{
	int ret, nr_nodes;
	struct sheepdog_node_list_entry entry[SD_MAX_NODES];

	nr_nodes = get_ordered_sd_node_list(entry);

	dprintf("update epoch, %d, %d\n", epoch, nr_nodes);
	ret = epoch_log_write(epoch, (char *)entry,
			nr_nodes * sizeof(struct sheepdog_node_list_entry));
	if (ret < 0)
		eprintf("can't write epoch %u\n", epoch);

	return ret;
}

static void update_cluster_info(struct join_message *msg)
{
	int i;
	int ret, nr_nodes = msg->nr_nodes;

	eprintf("status = %d, epoch = %d, %x, %d\n", msg->cluster_status, msg->epoch, msg->result, sys->join_finished);
	if (msg->result != SD_RES_SUCCESS) {
		if (is_myself(msg->header.from.addr, msg->header.from.port)) {
			eprintf("failed to join sheepdog, %d\n", msg->result);
			leave_cluster();
			eprintf("Restart me later when master is up, please.Bye.\n");
			exit(1);
			/* sys->status = SD_STATUS_JOIN_FAILED; */
		}
		return;
	}

	if (sys->status == SD_STATUS_JOIN_FAILED)
		return;

	if (!sys->nr_sobjs)
		sys->nr_sobjs = msg->nr_sobjs;

	if (sys->join_finished)
		goto join_finished;

	sys->epoch = msg->epoch;
	for (i = 0; i < nr_nodes; i++) {
		ret = move_node_to_sd_list(&msg->nodes[i].sheepid,
					   msg->nodes[i].ent);
		/*
		 * the node belonged to sheepdog when the master build
		 * the JOIN response however it has gone.
		 */
		if (ret)
			vprintf(SDOG_INFO, "%s has gone\n",
				sheepid_to_str(&msg->nodes[i].sheepid));
	}

	if (msg->cluster_status == SD_STATUS_WAIT_FOR_JOIN)
		add_node_to_leave_list((struct message_header *)msg);

	sys->join_finished = 1;

	if ((msg->cluster_status == SD_STATUS_OK || msg->cluster_status == SD_STATUS_HALT)
	     && msg->inc_epoch)
		update_epoch_log(sys->epoch);

join_finished:
	ret = move_node_to_sd_list(&msg->header.sheepid, msg->header.from);
	/*
	 * this should not happen since __sd_deliver() checks if the
	 * host from msg on cpg_node_list.
	 */
	if (ret)
		vprintf(SDOG_ERR, "%s has gone\n",
			sheepid_to_str(&msg->header.sheepid));

	if (msg->cluster_status == SD_STATUS_OK ||
	    msg->cluster_status == SD_STATUS_HALT) {
		if (msg->inc_epoch) {
			sys->epoch++;
			update_epoch_log(sys->epoch);
			update_epoch_store(sys->epoch);
		}

		if (sys->status != SD_STATUS_OK ||
		    sys->status != SD_STATUS_HALT) {
			set_global_nr_copies(sys->nr_sobjs);
			set_cluster_ctime(msg->ctime);
		}
	}

	print_node_list(&sys->sd_node_list);

	sys->status = msg->cluster_status;
	return;
}

static void vdi_op(struct vdi_op_message *msg)
{
	const struct sd_vdi_req *hdr = &msg->req;
	struct sd_vdi_rsp *rsp = &msg->rsp;
	void *data = msg->data;
	int ret = SD_RES_SUCCESS;
	uint32_t vid = 0, attrid = 0, nr_copies = sys->nr_sobjs;

	switch (hdr->opcode) {
	case SD_OP_NEW_VDI:
		ret = add_vdi(hdr->epoch, data, hdr->data_length, hdr->vdi_size, &vid,
			      hdr->base_vdi_id, hdr->copies,
			      hdr->snapid, &nr_copies);
		break;
	case SD_OP_DEL_VDI:
		ret = del_vdi(hdr->epoch, data, hdr->data_length, &vid,
			      hdr->snapid, &nr_copies);
		break;
	case SD_OP_LOCK_VDI:
	case SD_OP_GET_VDI_INFO:
		if (hdr->proto_ver != SD_PROTO_VER) {
			ret = SD_RES_VER_MISMATCH;
			break;
		}
		ret = lookup_vdi(hdr->epoch, data, hdr->data_length, &vid,
				 hdr->snapid, &nr_copies);
		if (ret != SD_RES_SUCCESS)
			break;
		break;
	case SD_OP_GET_VDI_ATTR:
		ret = lookup_vdi(hdr->epoch, data,
				 min(SD_MAX_VDI_LEN + SD_MAX_VDI_TAG_LEN, hdr->data_length),
				 &vid, hdr->snapid, &nr_copies);
		if (ret != SD_RES_SUCCESS)
			break;
		/* the curernt vdi id can change if we take the snapshot,
		   so we use the hash value of the vdi name as the vdi id */
		vid = fnv_64a_buf(data, strlen(data), FNV1A_64_INIT);
		vid &= SD_NR_VDIS - 1;
		ret = get_vdi_attr(hdr->epoch, data, hdr->data_length, vid,
				   &attrid, nr_copies,
				   hdr->flags & SD_FLAG_CMD_CREAT,
				   hdr->flags & SD_FLAG_CMD_EXCL);
		break;
	case SD_OP_RELEASE_VDI:
		break;
	case SD_OP_MAKE_FS:
		ret = SD_RES_SUCCESS;
		break;
	case SD_OP_SHUTDOWN:
		break;
	default:
		ret = SD_RES_SYSTEM_ERROR;
		eprintf("opcode %d is not implemented\n", hdr->opcode);
		break;
	}

	rsp->vdi_id = vid;
	rsp->attr_id = attrid;
	rsp->copies = nr_copies;
	rsp->result = ret;
}

static void vdi_op_done(struct vdi_op_message *msg)
{
	const struct sd_vdi_req *hdr = &msg->req;
	struct sd_vdi_rsp *rsp = &msg->rsp;
	void *data = msg->data;
	struct request *req;
	int ret = msg->rsp.result;
	int i, latest_epoch, nr_nodes;
	struct sheepdog_node_list_entry entry[SD_MAX_NODES];
	uint64_t ctime;

	if (ret != SD_RES_SUCCESS)
		goto out;

	switch (hdr->opcode) {
	case SD_OP_NEW_VDI:
	{
		unsigned long nr = rsp->vdi_id;
		vprintf(SDOG_INFO, "done %d %ld\n", ret, nr);
		set_bit(nr, sys->vdi_inuse);
		break;
	}
	case SD_OP_DEL_VDI:
		break;
	case SD_OP_LOCK_VDI:
	case SD_OP_RELEASE_VDI:
	case SD_OP_GET_VDI_INFO:
	case SD_OP_GET_VDI_ATTR:
		break;
	case SD_OP_MAKE_FS:
		sys->nr_sobjs = ((struct sd_so_req *)hdr)->copies;
		if (!sys->nr_sobjs)
			sys->nr_sobjs = SD_DEFAULT_REDUNDANCY;

		ctime = ((struct sd_so_req *)hdr)->ctime;
		set_cluster_ctime(ctime);

		latest_epoch = get_latest_epoch();
		for (i = 1; i <= latest_epoch; i++)
			remove_epoch(i);
		memset(sys->vdi_inuse, 0, sizeof(sys->vdi_inuse));

		sys->epoch = 1;
		sys->recovered_epoch = 1;
		nr_nodes = get_ordered_sd_node_list(entry);

		dprintf("write epoch log, %d, %d\n", sys->epoch, nr_nodes);
		ret = epoch_log_write(sys->epoch, (char *)entry,
				      nr_nodes * sizeof(struct sheepdog_node_list_entry));
		if (ret < 0)
			eprintf("can't write epoch %u\n", sys->epoch);
		update_epoch_store(sys->epoch);

		set_global_nr_copies(sys->nr_sobjs);

		sys->status = SD_STATUS_OK;
		break;
	case SD_OP_SHUTDOWN:
		sys->status = SD_STATUS_SHUTDOWN;
		break;
	default:
		eprintf("unknown operation %d\n", hdr->opcode);
		ret = SD_RES_UNKNOWN;
	}
out:
	if (!is_myself(msg->header.from.addr, msg->header.from.port))
		return;

	req = list_first_entry(&sys->pending_list, struct request, pending_list);

	rsp->result = ret;
	memcpy(req->data, data, rsp->data_length);
	memcpy(&req->rp, rsp, sizeof(req->rp));
	list_del(&req->pending_list);
	req->done(req);
}

static void __sd_notify(struct cpg_event *cevent)
{
	struct work_notify *w = container_of(cevent, struct work_notify, cev);
	struct message_header *m = w->msg;
	char name[128];
	struct node *node;

	dprintf("op: %d, state: %u, size: %d, from: %s, pid: %ld\n",
		m->op, m->state, m->msg_length,
		addr_to_str(name, sizeof(name), m->from.addr, m->from.port),
		m->sheepid.pid);

	/*
	 * we don't want to perform any deliver events except mastership_tx event
	 * until we join; we wait for our JOIN message.
	 */
	if (!sys->join_finished && !master_tx_message(m)) {
		if (sheepid_cmp(&m->sheepid, &sys->this_sheepid) != 0) {
			cevent->skip = 1;
			return;
		}
	}

	if (join_message(m)) {
		node = find_node(&sys->cpg_node_list, &m->sheepid);
		if (!node) {
			dprintf("the node was left before join operation is finished\n");
			return;
		}

		node->ent = m->from;
	}

	if (m->state == DM_INIT && is_master()) {
		switch (m->op) {
		case SD_MSG_JOIN:
			break;
		case SD_MSG_VDI_OP:
			vdi_op((struct vdi_op_message *)m);
			break;
		default:
			eprintf("unknown message %d\n", m->op);
			break;
		}
	}

	if (m->state == DM_FIN) {
		switch (m->op) {
		case SD_MSG_JOIN:
			if (((struct join_message *)m)->cluster_status == SD_STATUS_OK)
				if (sys->status != SD_STATUS_OK) {
					struct join_message *msg = (struct join_message *)m;
					int i;

					get_vdi_bitmap_from_sd_list();
					get_vdi_bitmap_from(&m->from);
					for (i = 0; i < msg->nr_nodes;i++)
						get_vdi_bitmap_from(&msg->nodes[i].ent);
			}
			break;
		}
	}

}

static int tx_mastership(void)
{
	struct mastership_tx_message msg;
	memset(&msg, 0, sizeof(msg));
	msg.header.proto_ver = SD_SHEEP_PROTO_VER;
	msg.header.op = SD_MSG_MASTER_TRANSFER;
	msg.header.state = DM_FIN;
	msg.header.msg_length = sizeof(msg);
	msg.header.from = sys->this_node;
	msg.header.sheepid = sys->this_sheepid;

	return sys->cdrv->notify(&msg, msg.header.msg_length);
}

static void send_join_response(struct work_notify *w)
{
	struct message_header *m;
	struct join_message *jm;
	struct node *node;

	m = w->msg;
	jm = (struct join_message *)m;
	join(jm);
	m->state = DM_FIN;

	dprintf("%d, %d\n", jm->result, jm->cluster_status);
	if (jm->result == SD_RES_SUCCESS &&
	    jm->cluster_status == SD_STATUS_WAIT_FOR_JOIN) {
		jm->nr_leave_nodes = 0;
		list_for_each_entry(node, &sys->leave_list, list) {
			jm->leave_nodes[jm->nr_leave_nodes].sheepid = node->sheepid;
			jm->leave_nodes[jm->nr_leave_nodes].ent = node->ent;
			jm->nr_leave_nodes++;
		}
		print_node_list(&sys->leave_list);
	} else if (jm->result != SD_RES_SUCCESS &&
			jm->epoch > sys->epoch &&
			jm->cluster_status == SD_STATUS_WAIT_FOR_JOIN) {
		eprintf("Transfer mastership.\n");
		tx_mastership();
		eprintf("Restart me later when master is up, please.Bye.\n");
		exit(1);
	}
	jm->epoch = sys->epoch;
	sys->cdrv->notify(m, m->msg_length);
}

static void __sd_notify_done(struct cpg_event *cevent)
{
	struct work_notify *w = container_of(cevent, struct work_notify, cev);
	struct message_header *m;
	char name[128];
	int do_recovery;
	struct node *node, *t;
	int nr, nr_local, nr_leave;

	m = w->msg;

	if (m->state == DM_FIN) {
		switch (m->op) {
		case SD_MSG_JOIN:
			update_cluster_info((struct join_message *)m);
			break;
		case SD_MSG_LEAVE:
			node = find_node(&sys->sd_node_list, &m->sheepid);
			if (node) {
				sys->nr_vnodes = 0;

				list_del(&node->list);
				free(node);
				if (sys->status == SD_STATUS_OK) {
					sys->epoch++;
					update_epoch_log(sys->epoch);
					update_epoch_store(sys->epoch);
				}
			}
		/* fall through */
		case SD_MSG_MASTER_TRANSFER:
			if (sys->status == SD_STATUS_WAIT_FOR_JOIN) {
				add_node_to_leave_list(m);

				/* Sheep needs this to identify itself as master.
				 * Now mastership transfer is done.
				 */
				if (!sys->join_finished) {
					sys->join_finished = 1;
					move_node_to_sd_list(&sys->this_sheepid, sys->this_node);
					sys->epoch = get_latest_epoch();
				}

				nr_local = get_nodes_nr_epoch(sys->epoch);
				nr = get_nodes_nr_from(&sys->sd_node_list);
				nr_leave = get_nodes_nr_from(&sys->leave_list);

				dprintf("%d == %d + %d \n", nr_local, nr, nr_leave);
				if (nr_local == nr + nr_leave) {
					sys->status = SD_STATUS_OK;
					update_epoch_log(sys->epoch);
					update_epoch_store(sys->epoch);
				}
			}
			break;
		case SD_MSG_VDI_OP:
			break;
		default:
			eprintf("unknown message %d\n", m->op);
			break;
		}
	}

	do_recovery = (m->state == DM_FIN &&
		       (join_message(m) || leave_message(m)));

	dprintf("op: %d, state: %u, size: %d, from: %s\n",
		m->op, m->state, m->msg_length,
		addr_to_str(name, sizeof(name), m->from.addr,
			    m->from.port));

	if (m->state == DM_INIT && is_master()) {
		switch (m->op) {
		case SD_MSG_JOIN:
			send_join_response(w);
			break;
		case SD_MSG_VDI_OP:
			m->state = DM_FIN;
			sys->cdrv->notify(m, m->msg_length);
			break;
		default:
			eprintf("unknown message %d\n", m->op);
			break;
		}
	}

	if (do_recovery &&
	    (sys->status == SD_STATUS_OK || sys->status == SD_STATUS_HALT)) {
		list_for_each_entry_safe(node, t, &sys->leave_list, list) {
			list_del(&node->list);
		}
		start_recovery(sys->epoch);
	}
}

static void sd_notify_handler(struct sheepid *sender, void *msg, size_t msg_len)
{
	struct cpg_event *cevent;
	struct work_notify *w;
	struct message_header *m = msg;
	char name[128];

	dprintf("op: %d, state: %u, size: %d, from: %s, pid: %lu\n",
		m->op, m->state, m->msg_length,
		addr_to_str(name, sizeof(name), m->from.addr, m->from.port),
		sender->pid);

	w = zalloc(sizeof(*w));
	if (!w)
		return;

	cevent = &w->cev;
	cevent->ctype = CPG_EVENT_NOTIFY;

	vprintf(SDOG_DEBUG, "allow new deliver, %p\n", cevent);

	w->msg = zalloc(msg_len);
	if (!w->msg)
		return;
	memcpy(w->msg, msg, msg_len);

	if (cpg_event_suspended() && m->state == DM_FIN) {
		list_add(&cevent->cpg_event_list, &sys->cpg_event_siblings);
		cpg_event_clear_suspended();
		if (join_message(m))
			cpg_event_clear_joining();
	} else
		list_add_tail(&cevent->cpg_event_list, &sys->cpg_event_siblings);

	start_cpg_event_work();
}

static void add_node(struct sheepid *id)
{
	struct node *node;

	node = zalloc(sizeof(*node));
	if (!node)
		panic("failed to alloc memory for a new node\n");

	node->sheepid = *id;

	list_add_tail(&node->list, &sys->cpg_node_list);
}

static int del_node(struct sheepid *id)
{
	struct node *node;

	node = find_node(&sys->sd_node_list, id);
	if (node) {
		int nr;
		struct sheepdog_node_list_entry e[SD_MAX_NODES];

		sys->nr_vnodes = 0;

		list_del(&node->list);
		free(node);

		if (sys->status == SD_STATUS_OK ||
		    sys->status == SD_STATUS_HALT) {
			nr = get_ordered_sd_node_list(e);
			dprintf("update epoch, %d, %d\n", sys->epoch + 1, nr);
			epoch_log_write(sys->epoch + 1, (char *)e,
					nr * sizeof(struct sheepdog_node_list_entry));

			sys->epoch++;

			update_epoch_store(sys->epoch);
		}
		return 1;
	}

	node = find_node(&sys->cpg_node_list, id);
	if (node) {
		list_del(&node->list);
		free(node);
	}

	return 0;
}

/*
 * Check whether the majority of Sheepdog nodes are still alive or not
 */
static int check_majority(struct sheepid *left)
{
	int nr_nodes = 0, nr_majority, nr_reachable = 0, fd;
	struct node *node;
	char name[INET6_ADDRSTRLEN];

	nr_nodes = get_nodes_nr_from(&sys->sd_node_list);
	nr_majority = nr_nodes / 2 + 1;

	/* we need at least 3 nodes to handle network partition
	 * failure */
	if (nr_nodes < 3)
		return 1;

	list_for_each_entry(node, &sys->sd_node_list, list) {
		if (sheepid_cmp(&node->sheepid, left) == 0)
			continue;

		addr_to_str(name, sizeof(name), node->ent.addr, 0);
		fd = connect_to(name, node->ent.port);
		if (fd < 0)
			continue;

		close(fd);
		nr_reachable++;
		if (nr_reachable >= nr_majority) {
			dprintf("majority nodes are alive\n");
			return 1;
		}
	}
	dprintf("%d, %d, %d\n", nr_nodes, nr_majority, nr_reachable);
	eprintf("majority nodes are not alive\n");
	return 0;
}

static void __sd_leave(struct cpg_event *cevent)
{
	struct work_leave *w = container_of(cevent, struct work_leave, cev);

	if (!check_majority(&w->left)) {
		eprintf("perhaps network partition failure has occurred\n");
		abort();
	}
}

static void send_join_request(struct sheepid *id)
{
	struct join_message msg;
	struct sheepdog_node_list_entry entries[SD_MAX_NODES];
	int nr_entries, i, ret;

	memset(&msg, 0, sizeof(msg));
	msg.header.proto_ver = SD_SHEEP_PROTO_VER;
	msg.header.op = SD_MSG_JOIN;
	msg.header.state = DM_INIT;
	msg.header.msg_length = sizeof(msg);
	msg.header.from = sys->this_node;
	msg.header.sheepid = sys->this_sheepid;

	get_global_nr_copies(&msg.nr_sobjs);

	nr_entries = ARRAY_SIZE(entries);
	ret = read_epoch(&msg.epoch, &msg.ctime, entries, &nr_entries);
	if (ret == SD_RES_SUCCESS) {
		msg.nr_nodes = nr_entries;
		for (i = 0; i < nr_entries; i++)
			msg.nodes[i].ent = entries[i];
	}

	sys->cdrv->notify(&msg, msg.header.msg_length);

	vprintf(SDOG_INFO, "%s\n", sheepid_to_str(&sys->this_sheepid));
}

static void __sd_join_done(struct cpg_event *cevent)
{
	struct work_join *w = container_of(cevent, struct work_join, cev);
	int ret, i;
	int first_cpg_node = 0;

	if (w->member_list_entries == 1 &&
	    sheepid_cmp(&w->joined, &sys->this_sheepid) == 0) {
		sys->join_finished = 1;
		get_global_nr_copies(&sys->nr_sobjs);
		first_cpg_node = 1;
	}

	if (list_empty(&sys->cpg_node_list)) {
		for (i = 0; i < w->member_list_entries; i++)
			add_node(w->member_list + i);
	} else
		add_node(&w->joined);

	if (first_cpg_node) {
		struct join_message msg;
		struct sheepdog_node_list_entry entries[SD_MAX_NODES];
		int nr_entries;
		uint64_t ctime;
		uint32_t epoch;

		/*
		 * If I'm the first sheep joins in colosync, I
		 * becomes the master without sending JOIN.
		 */

		vprintf(SDOG_DEBUG, "%s\n", sheepid_to_str(&sys->this_sheepid));

		memset(&msg, 0, sizeof(msg));

		msg.header.from = sys->this_node;
		msg.header.sheepid = sys->this_sheepid;

		nr_entries = ARRAY_SIZE(entries);
		ret = read_epoch(&epoch, &ctime, entries, &nr_entries);
		if (ret == SD_RES_SUCCESS) {
			sys->epoch = epoch;
			msg.ctime = ctime;
			get_cluster_status(&msg.header.from, entries, nr_entries,
					   ctime, epoch, &msg.cluster_status, NULL);
		} else
			msg.cluster_status = SD_STATUS_WAIT_FOR_FORMAT;

		update_cluster_info(&msg);

		if (sys->status == SD_STATUS_OK) /* sheepdog starts with one node */
			start_recovery(sys->epoch);

		return;
	}

	print_node_list(&sys->sd_node_list);

	if (sheepid_cmp(&w->joined, &sys->this_sheepid) == 0)
		send_join_request(&w->joined);
}

static void __sd_leave_done(struct cpg_event *cevent)
{
	struct work_leave *w = container_of(cevent, struct work_leave, cev);
	int node_left;

	node_left = del_node(&w->left);

	print_node_list(&sys->sd_node_list);

	if (node_left &&
	    (sys->status == SD_STATUS_OK || sys->status == SD_STATUS_HALT))
		start_recovery(sys->epoch);
}

static void cpg_event_free(struct cpg_event *cevent)
{
	switch (cevent->ctype) {
	case CPG_EVENT_JOIN: {
		struct work_join *w = container_of(cevent, struct work_join, cev);
		free(w->member_list);
		free(w);
		break;
	}
	case CPG_EVENT_LEAVE: {
		struct work_leave *w = container_of(cevent, struct work_leave, cev);
		free(w->member_list);
		free(w);
		break;
	}
	case CPG_EVENT_NOTIFY: {
		struct work_notify *w = container_of(cevent, struct work_notify, cev);
		free(w->msg);
		free(w);
		break;
	}
	default:
		break;
	}
}

static struct work cpg_event_work;

static void cpg_event_fn(struct work *work, int idx)
{
	struct cpg_event *cevent = sys->cur_cevent;

	vprintf(SDOG_DEBUG, "%p, %d %lx\n", cevent, cevent->ctype,
		sys->cpg_event_work_flags);

	/*
	 * we can't touch sys->cpg_event_siblings because of a race
	 * with sd_deliver() and sd_confchg()...
	 */

	switch (cevent->ctype) {
	case CPG_EVENT_JOIN:
		break;
	case CPG_EVENT_LEAVE:
		__sd_leave(cevent);
		break;
	case CPG_EVENT_NOTIFY:
	{
		struct work_notify *w = container_of(cevent, struct work_notify, cev);
		vprintf(SDOG_DEBUG, "%d\n", w->msg->state);
		__sd_notify(cevent);
		break;
	}
	case CPG_EVENT_REQUEST:
		vprintf(SDOG_ERR, "should not happen\n");
		break;
	default:
		vprintf(SDOG_ERR, "unknown event %d\n", cevent->ctype);
	}
}

static void cpg_event_done(struct work *work, int idx)
{
	struct cpg_event *cevent;

	if (!sys->cur_cevent)
		vprintf(SDOG_ERR, "bug\n");

	cevent = sys->cur_cevent;
	sys->cur_cevent = NULL;

	vprintf(SDOG_DEBUG, "%p\n", cevent);

	if (cpg_event_suspended())
		goto out;

	if (cevent->skip)
		goto out;

	switch (cevent->ctype) {
	case CPG_EVENT_JOIN:
		__sd_join_done(cevent);
		break;
	case CPG_EVENT_LEAVE:
		__sd_leave_done(cevent);
		break;
	case CPG_EVENT_NOTIFY:
	{
		struct work_notify *w = container_of(cevent, struct work_notify, cev);

		if (w->msg->state == DM_FIN && vdi_op_message(w->msg))
			vdi_op_done((struct vdi_op_message *)w->msg);

		/*
		 * if we are in the process of the JOIN, we will not
		 * be suspended. So sd_deliver() links events to
		 * cpg_event_siblings in order. The events except for
		 * JOIN with DM_CONT and DM_FIN are skipped.
		 */
		if (sys->join_finished && w->msg->state == DM_INIT) {
			struct cpg_event *f_cevent;

			list_for_each_entry(f_cevent, &sys->cpg_event_siblings,
					    cpg_event_list) {
				struct work_notify *fw =
					container_of(f_cevent, struct work_notify, cev);
				if (f_cevent->ctype == CPG_EVENT_NOTIFY &&
				    fw->msg->state == DM_FIN) {
					vprintf(SDOG_INFO, "already got fin %p\n",
						f_cevent);

					list_del(&f_cevent->cpg_event_list);
					list_add(&f_cevent->cpg_event_list,
						 &sys->cpg_event_siblings);
					goto got_fin;
				}
			}
			cpg_event_set_suspended();
			if (join_message(w->msg))
				cpg_event_set_joining();
		}
	got_fin:
		__sd_notify_done(cevent);
		break;
	}
	case CPG_EVENT_REQUEST:
		vprintf(SDOG_ERR, "should not happen\n");
		break;
	default:
		vprintf(SDOG_ERR, "unknown event %d\n", cevent->ctype);
	}

out:
	vprintf(SDOG_DEBUG, "free %p\n", cevent);
	cpg_event_free(cevent);
	cpg_event_clear_running();

	if (!list_empty(&sys->cpg_event_siblings)) {
		if (cpg_event_joining())
			/* io requests need to return SD_RES_NEW_NODE_VER */
			start_cpg_event_work();
		else if (!cpg_event_suspended())
			start_cpg_event_work();
	}
}

static int check_epoch(struct request *req)
{
	uint32_t req_epoch = req->rq.epoch;
	uint32_t opcode = req->rq.opcode;
	int ret = SD_RES_SUCCESS;

	if (before(req_epoch, sys->epoch)) {
		ret = SD_RES_OLD_NODE_VER;
		eprintf("old node version %u %u, %x\n",
			sys->epoch, req_epoch, opcode);
	} else if (after(req_epoch, sys->epoch)) {
		ret = SD_RES_NEW_NODE_VER;
			eprintf("new node version %u %u %x\n",
				sys->epoch, req_epoch, opcode);
	}
	return ret;
}

int is_access_to_busy_objects(uint64_t oid)
{
	struct request *req;

	if (!oid)
		return 0;

	list_for_each_entry(req, &sys->outstanding_req_list, r_wlist) {
		if (req->rq.flags & SD_FLAG_CMD_RECOVERY) {
			if (req->rq.opcode != SD_OP_READ_OBJ)
				eprintf("bug\n");
			continue;
		}
		if (oid == req->local_oid)
				return 1;
	}
	return 0;
}

static int __is_access_to_recoverying_objects(struct request *req)
{
	if (req->rq.flags & SD_FLAG_CMD_RECOVERY) {
		if (req->rq.opcode != SD_OP_READ_OBJ)
			eprintf("bug\n");
		return 0;
	}

	if (is_recoverying_oid(req->local_oid))
		return 1;

	return 0;
}

static int __is_access_to_busy_objects(struct request *req)
{
	if (req->rq.flags & SD_FLAG_CMD_RECOVERY) {
		if (req->rq.opcode != SD_OP_READ_OBJ)
			eprintf("bug\n");
		return 0;
	}

	if (is_access_to_busy_objects(req->local_oid))
		return 1;

	return 0;
}

/* can be called only by the main process */
void start_cpg_event_work(void)
{
	struct cpg_event *cevent, *n;
	LIST_HEAD(failed_req_list);
	int retry;

	if (list_empty(&sys->cpg_event_siblings))
		vprintf(SDOG_ERR, "bug\n");

	cevent = list_first_entry(&sys->cpg_event_siblings,
				  struct cpg_event, cpg_event_list);

	vprintf(SDOG_DEBUG, "%lx %u\n", sys->cpg_event_work_flags,
		cevent->ctype);

	/*
	 * we need to serialize cpg events so we don't call queue_work
	 * if a thread is still running for a cpg event; executing
	 * cpg_event_fn() or cpg_event_done(). A exception: if a
	 * thread is running for a deliver for VDI, then we need to
	 * run io requests.
	 */
	if (cpg_event_running() && is_membership_change_event(cevent->ctype))
		return;

	/*
	 * we are in the processing of handling JOIN so we can't
	 * execute requests (or cpg events).
	 */
	if (cpg_event_joining()) {
		if (!cpg_event_suspended())
			panic("should not happen\n");

		if (cevent->ctype == CPG_EVENT_REQUEST) {
			struct request *req = container_of(cevent, struct request, cev);
			if (is_io_request(req->rq.opcode) && req->rq.flags & SD_FLAG_CMD_DIRECT) {
				list_del(&cevent->cpg_event_list);

				req->rp.result = SD_RES_NEW_NODE_VER;

				/* TODO: cleanup */
				list_add_tail(&req->r_wlist, &sys->outstanding_req_list);
				sys->nr_outstanding_io++;

				req->work.done(&req->work, 0);
			}
		}
		return;
	}

do_retry:
	retry = 0;

	list_for_each_entry_safe(cevent, n, &sys->cpg_event_siblings, cpg_event_list) {
		struct request *req = container_of(cevent, struct request, cev);

		if (cevent->ctype == CPG_EVENT_NOTIFY)
			continue;
		if (is_membership_change_event(cevent->ctype))
			break;

		list_del(&cevent->cpg_event_list);

		if (is_io_request(req->rq.opcode)) {
			int copies = sys->nr_sobjs;

			if (copies > req->nr_zones)
				copies = req->nr_zones;

			if (__is_access_to_recoverying_objects(req)) {
				if (req->rq.flags & SD_FLAG_CMD_DIRECT) {
					req->rp.result = SD_RES_NEW_NODE_VER;
					sys->nr_outstanding_io++; /* TODO: cleanup */
					list_add_tail(&req->r_wlist, &failed_req_list);
				} else
					list_add_tail(&req->r_wlist, &sys->req_wait_for_obj_list);
				continue;
			}
			if (__is_access_to_busy_objects(req)) {
				list_add_tail(&req->r_wlist, &sys->req_wait_for_obj_list);
				continue;
			}

			list_add_tail(&req->r_wlist, &sys->outstanding_req_list);

			sys->nr_outstanding_io++;

			if (is_access_local(req->entry, req->nr_vnodes,
					    ((struct sd_obj_req *)&req->rq)->oid, copies) ||
			    is_access_local(req->entry, req->nr_vnodes,
					    ((struct sd_obj_req *)&req->rq)->cow_oid, copies)) {
				int ret = check_epoch(req);
				if (ret != SD_RES_SUCCESS) {
					req->rp.result = ret;
					list_del(&req->r_wlist);
					list_add_tail(&req->r_wlist, &failed_req_list);
					continue;
				}
			}

			if (!(req->rq.flags & SD_FLAG_CMD_DIRECT) &&
			    req->rq.opcode == SD_OP_READ_OBJ) {
				struct sd_obj_req *hdr = (struct sd_obj_req *)&req->rq;
				uint32_t vdi_id = oid_to_vid(hdr->oid);
				struct data_object_bmap *bmap;

				req->check_consistency = 1;
				if (!is_vdi_obj(hdr->oid)) {
					list_for_each_entry(bmap, &sys->consistent_obj_list, list) {
						if (bmap->vdi_id == vdi_id) {
							if (test_bit(data_oid_to_idx(hdr->oid), bmap->dobjs))
								req->check_consistency = 0;
							break;
						}
					}
				}
			}
		}
		if (req->rq.flags & SD_FLAG_CMD_DIRECT)
			queue_work(sys->io_wqueue, &req->work);
		else
			queue_work(sys->gateway_wqueue, &req->work);
	}

	while (!list_empty(&failed_req_list)) {
		struct request *req = list_first_entry(&failed_req_list,
						       struct request, r_wlist);
		req->work.done(&req->work, 0);

		retry = 1;
	}

	if (retry)
		goto do_retry;

	if (cpg_event_running() || cpg_event_suspended() ||
	    list_empty(&sys->cpg_event_siblings))
		return;

	cevent = list_first_entry(&sys->cpg_event_siblings,
				  struct cpg_event, cpg_event_list);

	if (is_membership_change_event(cevent->ctype) && sys->nr_outstanding_io)
		return;

	list_del(&cevent->cpg_event_list);
	sys->cur_cevent = cevent;

	cpg_event_set_running();

	INIT_LIST_HEAD(&cpg_event_work.w_list);
	cpg_event_work.fn = cpg_event_fn;
	cpg_event_work.done = cpg_event_done;

	queue_work(sys->cpg_wqueue, &cpg_event_work);
}

static void sd_join_handler(struct sheepid *joined, struct sheepid *members,
			    size_t nr_members)
{
	struct cpg_event *cevent;
	struct work_join *w = NULL;
	int i, size;

	dprintf("join %s\n", sheepid_to_str(joined));
	for (i = 0; i < nr_members; i++)
		dprintf("[%x] %s\n", i, sheepid_to_str(members + i));

	if (sys->status == SD_STATUS_SHUTDOWN)
		return;

	w = zalloc(sizeof(*w));
	if (!w)
		goto oom;

	cevent = &w->cev;
	cevent->ctype = CPG_EVENT_JOIN;


	vprintf(SDOG_DEBUG, "allow new confchg, %p\n", cevent);

	size = sizeof(struct sheepid) * nr_members;
	w->member_list = zalloc(size);
	if (!w->member_list)
		goto oom;
	memcpy(w->member_list, members, size);
	w->member_list_entries = nr_members;

	w->joined = *joined;

	list_add_tail(&cevent->cpg_event_list, &sys->cpg_event_siblings);
	start_cpg_event_work();

	return;
oom:
	if (w) {
		if (w->member_list)
			free(w->member_list);
		free(w);
	}
	panic("failed to allocate memory for a confchg event\n");
}

static void sd_leave_handler(struct sheepid *left, struct sheepid *members,
			     size_t nr_members)
{
	struct cpg_event *cevent;
	struct work_leave *w = NULL;
	int i, size;

	dprintf("leave %s\n", sheepid_to_str(left));
	for (i = 0; i < nr_members; i++)
		dprintf("[%x] %s\n", i, sheepid_to_str(members + i));

	if (sys->status == SD_STATUS_SHUTDOWN)
		return;

	w = zalloc(sizeof(*w));
	if (!w)
		goto oom;

	cevent = &w->cev;
	cevent->ctype = CPG_EVENT_LEAVE;


	vprintf(SDOG_DEBUG, "allow new confchg, %p\n", cevent);

	size = sizeof(struct sheepid) * nr_members;
	w->member_list = zalloc(size);
	if (!w->member_list)
		goto oom;
	memcpy(w->member_list, members, size);
	w->member_list_entries = nr_members;

	w->left = *left;

	list_add_tail(&cevent->cpg_event_list, &sys->cpg_event_siblings);
	start_cpg_event_work();

	return;
oom:
	if (w) {
		if (w->member_list)
			free(w->member_list);
		free(w);
	}
	panic("failed to allocate memory for a confchg event\n");
}

int create_cluster(int port, int64_t zone)
{
	int fd, ret;
	struct cluster_driver *cdrv;
	struct cdrv_handlers handlers = {
		.join_handler = sd_join_handler,
		.leave_handler = sd_leave_handler,
		.notify_handler = sd_notify_handler,
	};

	if (!sys->cdrv) {
		FOR_EACH_CLUSTER_DRIVER(cdrv) {
			if (strcmp(cdrv->name, "corosync") == 0) {
				dprintf("use corosync driver as default\n");
				sys->cdrv = cdrv;
				break;
			}
		}
	}

	fd = sys->cdrv->init(&handlers, &sys->this_sheepid);
	if (fd < 0)
		return -1;

	ret = sys->cdrv->join();
	if (ret != 0)
		return -1;

	memcpy(sys->this_node.addr, sys->this_sheepid.addr,
	       sizeof(sys->this_node.addr));
	sys->this_node.port = port;
	sys->this_node.nr_vnodes = SD_DEFAULT_VNODES;
	if (zone == -1) {
		/* use last 4 bytes as zone id */
		uint8_t *b = sys->this_sheepid.addr + 12;
		sys->this_node.zone = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
	} else
		sys->this_node.zone = zone;
	dprintf("zone id = %u\n", sys->this_node.zone);

	if (get_latest_epoch() == 0)
		sys->status = SD_STATUS_WAIT_FOR_FORMAT;
	else
		sys->status = SD_STATUS_WAIT_FOR_JOIN;
	INIT_LIST_HEAD(&sys->sd_node_list);
	INIT_LIST_HEAD(&sys->cpg_node_list);
	INIT_LIST_HEAD(&sys->pending_list);
	INIT_LIST_HEAD(&sys->leave_list);

	INIT_LIST_HEAD(&sys->outstanding_req_list);
	INIT_LIST_HEAD(&sys->req_wait_for_obj_list);
	INIT_LIST_HEAD(&sys->consistent_obj_list);

	INIT_LIST_HEAD(&sys->cpg_event_siblings);

	ret = register_event(fd, group_handler, NULL);
	if (ret) {
		eprintf("Failed to register epoll events, %d\n", ret);
		return 1;
	}
	return 0;
}

/* after this function is called, this node only works as a gateway */
int leave_cluster(void)
{
	struct leave_message msg;

	memset(&msg, 0, sizeof(msg));
	msg.header.proto_ver = SD_SHEEP_PROTO_VER;
	msg.header.op = SD_MSG_LEAVE;
	msg.header.state = DM_FIN;
	msg.header.msg_length = sizeof(msg);
	msg.header.from = sys->this_node;
	msg.header.sheepid = sys->this_sheepid;
	msg.epoch = get_latest_epoch();

	dprintf("%d\n", msg.epoch);
	return sys->cdrv->notify(&msg, msg.header.msg_length);
}
