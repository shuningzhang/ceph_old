
#include <linux/ceph_fs.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/random.h>

/* debug level; defined in include/ceph_fs.h */
int ceph_debug = 0;

int ceph_client_debug = 50;
#define DOUT_VAR ceph_client_debug
#define DOUT_PREFIX "client: "
#include "super.h"
#include "ktcp.h"


void ceph_dispatch(void *p, struct ceph_msg *msg);


/*
 * share work queue between clients.
 */
static spinlock_t ceph_client_spinlock = SPIN_LOCK_UNLOCKED;
static int ceph_num_clients = 0;

static void get_client_counter(void) 
{
	spin_lock(&ceph_client_spinlock);
	if (ceph_num_clients == 0) {
		dout(1, "first client, setting up workqueues\n");
		ceph_workqueue_init();
	}
	ceph_num_clients++;
	spin_unlock(&ceph_client_spinlock);
}

static void put_client_counter(void) 
{
	spin_lock(&ceph_client_spinlock);
	ceph_num_clients--;
	if (ceph_num_clients == 0) {
		dout(1, "last client, shutting down workqueues\n");
		ceph_workqueue_shutdown();
	}
	spin_unlock(&ceph_client_spinlock);
}


int parse_open_reply(struct ceph_msg *reply, struct inode *inode, struct ceph_mds_session *session)
{
	struct ceph_mds_reply_head *head;
	struct ceph_mds_reply_info rinfo;
	int frommds = session->s_mds;
	int err;
	struct ceph_inode_cap *cap;

	/* parse reply */
	head = reply->front.iov_base;
	err = le32_to_cpu(head->result);
	dout(30, "parse_open_reply mds%d reports %d\n", frommds, err);
	if (err < 0) 
		return err;
	if ((err = ceph_mdsc_parse_reply_info(reply, &rinfo)) < 0)
		return err;
	BUG_ON(rinfo.trace_nr == 0);
	if ((err = ceph_fill_inode(inode, rinfo.trace_in[rinfo.trace_nr-1].in)) < 0) 
		return err;

	/* fill in cap */
	cap = ceph_add_cap(inode, session, 
			   le32_to_cpu(head->file_caps), 
			   le32_to_cpu(head->file_caps_seq));
	if (IS_ERR(cap))
		return PTR_ERR(cap);

	ceph_mdsc_destroy_reply_info(&rinfo);
	return 0;
}

static int open_root_inode(struct ceph_client *client, struct ceph_mount_args *args, struct dentry **pmnt_root)
{
	struct ceph_mds_client *mdsc = &client->mdsc;
	struct inode *root_inode, *mnt_inode = NULL;
	struct ceph_msg *req = 0;
	struct ceph_mds_request_head *reqhead;
	struct ceph_mds_reply_info rinfo;
	struct ceph_mds_session *session;
	int frommds;
	int err;
	struct ceph_inode_cap *cap;
	struct ceph_inode_info *ci;
	int alloc_fs = 0;

	/* open dir */
	dout(30, "open_root_inode opening '%s'\n", args->path);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_OPEN, 1, args->path, 0, 0);
	if (IS_ERR(req)) 
		return PTR_ERR(req);
	reqhead = req->front.iov_base;
	reqhead->args.open.flags = O_DIRECTORY;
	reqhead->args.open.mode = 0;
	if ((err = ceph_mdsc_do_request(mdsc, req, &rinfo, &session)) < 0)
		return err;
	
	err = le32_to_cpu(rinfo.head->result);
	if (err != 0) 
		return err;
	if (rinfo.trace_nr == 0) {
		dout(10, "open_root_inode wtf, mds returns 0 but no trace\n");
		err = -EINVAL;
		goto out;
	}
	
	if (client->sb->s_root == NULL) {
		/* get the fs root inode. Note that this is not necessarily the root of
		   the mount */
		err = ceph_get_inode(client->sb, le64_to_cpu(rinfo.trace_in[0].in->ino), &root_inode);
		if (err < 0) 
			goto out;

		alloc_fs = 1;

		client->sb->s_root = d_alloc_root(root_inode);
		if (client->sb->s_root == NULL) {
			err = -ENOMEM;
			/* fixme: also close? */
			goto out2;
		}
	} else {
		root_inode = client->sb->s_root->d_inode;
		BUG_ON (root_inode == NULL);
	}

	if ((err = ceph_fill_trace(client->sb, &rinfo, &mnt_inode, pmnt_root)) < 0)
		goto out2;
	if (*pmnt_root == NULL) {
		err = -ENOMEM;
		goto out2;
	}

	/* fill in cap */
	frommds = rinfo.reply->hdr.src.name.num;
	cap = ceph_add_cap(mnt_inode, session, 
			   le32_to_cpu(rinfo.head->file_caps), 
			   le32_to_cpu(rinfo.head->file_caps_seq));
	if (IS_ERR(cap)) {
		err = PTR_ERR(cap);
		goto out2;
	}

	ci = ceph_inode(mnt_inode);
	ci->i_nr_by_mode[FILE_MODE_PIN]++;

	dout(30, "open_root_inode success, root dentry is %p.\n", client->sb->s_root);
	return 0;

out2:
	dout(30, "open_root_inode failure %d\n", err);
	if (alloc_fs)
		iput(root_inode);
	iput(mnt_inode);
out:
	ceph_mdsc_put_session(session);
	return err;
}

/*
 * mount: join the ceph cluster.
 */
int ceph_mount(struct ceph_client *client, struct ceph_mount_args *args, struct dentry **pdroot)
{
	struct ceph_msg *mount_msg;
	int err;
	int attempts = 10;
	int which;
	char r;

	dout(10, "mount start\n");
	while (client->mounting < 7) {
		get_random_bytes(&r, 1);
		which = r % args->num_mon;
		mount_msg = ceph_msg_new(CEPH_MSG_CLIENT_MOUNT, 0, 0, 0, 0);
		if (IS_ERR(mount_msg))
			return PTR_ERR(mount_msg);
		mount_msg->hdr.dst.name.type = CEPH_ENTITY_TYPE_MON;
		mount_msg->hdr.dst.name.num = which;
		mount_msg->hdr.dst.addr = args->mon_addr[which];
		
		ceph_msg_send(client->msgr, mount_msg, 0);
		dout(10, "mount from mon%d, %d attempts left\n", which, attempts);
		
		/* wait */
		dout(10, "mount sent mount request, waiting for maps\n");
		err = wait_for_completion_timeout(&client->mount_completion, 6*HZ);
		if (err == -EINTR)
			return err; 
		if (client->mounting == 7) 
			break;  /* success */
		dout(10, "mount still waiting for mount, attempts=%d\n", attempts);
		if (--attempts == 0)
			return -EIO;
	}

	dout(30, "mount opening base mountpoint\n");
	if ((err = open_root_inode(client, args, pdroot)) < 0) 
		return err;

	dout(10, "mount success\n");
	return 0;
}


/*
 * the monitor responds to monmap to indicate mount success.
 * (or, someday, to indicate a change in the monitor cluster?)
 */
static void handle_monmap(struct ceph_client *client, struct ceph_msg *msg)
{
	int err;
	int first = (client->monc.monmap->epoch == 0);
	void *new;

	dout(1, "handle_monmap had epoch %d\n", client->monc.monmap->epoch);
	new = ceph_monmap_decode(msg->front.iov_base, 
				 msg->front.iov_base + msg->front.iov_len);
	if (IS_ERR(new)) {
		err = PTR_ERR(new);
		derr(0, "problem decoding monmap, %d\n", err);
		return;
	}
	kfree(client->monc.monmap);
	client->monc.monmap = new;

	if (first) {
		client->whoami = msg->hdr.dst.name.num;
		client->msgr->inst.name = msg->hdr.dst.name;
		dout(1, "i am client%d\n", client->whoami);
	}
}


void got_first_map(struct ceph_client *client, int num)
{
	set_bit(num, &client->mounting);
	dout(10, "got_first_map num %d mounting now %lu bits %d\n", 
	     num, client->mounting, (int)find_first_zero_bit(&client->mounting, 4));
	if (find_first_zero_bit(&client->mounting, 4) == 3) {
		dout(10, "got_first_map kicking mount\n");
		complete(&client->mount_completion);
	}
}



/*
 * create a fresh client instance
 */
struct ceph_client *ceph_create_client(struct ceph_mount_args *args, struct super_block *sb)
{
	struct ceph_client *cl;
	struct ceph_entity_addr *myaddr = 0;
	int err;

	cl = kzalloc(sizeof(*cl), GFP_KERNEL);
	if (cl == NULL)
		return ERR_PTR(-ENOMEM);

	init_completion(&cl->mount_completion);
	spin_lock_init(&cl->sb_lock);
	get_client_counter();

	/* messenger */
	if (args->flags & CEPH_MOUNT_MYIP)
		myaddr = &args->my_addr;
	cl->msgr = ceph_messenger_create(myaddr);
	if (IS_ERR(cl->msgr)) {
		err = PTR_ERR(cl->msgr);
		goto fail;
	}
	cl->msgr->parent = cl;
	cl->msgr->dispatch = ceph_dispatch;
	cl->msgr->prepare_pages = ceph_osdc_prepare_pages;
	
	cl->whoami = -1;
	if ((err = ceph_monc_init(&cl->monc, cl)) < 0)
		goto fail;
	ceph_mdsc_init(&cl->mdsc, cl);
	ceph_osdc_init(&cl->osdc, cl);

	cl->sb = sb;
	cl->mounting = 0;  /* wait for mon+mds+osd */

	return cl;

fail:
	put_client_counter();
	kfree(cl);
	return ERR_PTR(err);
}

void ceph_destroy_client(struct ceph_client *cl)
{
	dout(10, "destroy_client %p\n", cl);

	/* unmount */
	/* ... */

	ceph_mdsc_stop(&cl->mdsc);	

	ceph_messenger_destroy(cl->msgr);
	put_client_counter();
	kfree(cl);
	dout(10, "destroy_client %p done\n", cl);
}


/*
 * dispatch -- called with incoming messages.
 *
 * should be fast and non-blocking, as it is called with locks held.
 */
void ceph_dispatch(void *p, struct ceph_msg *msg)
{
	struct ceph_client *client = p;
	int had;

	/* deliver the message */
	switch (msg->hdr.type) {
		/* me */
	case CEPH_MSG_MON_MAP:
		had = client->monc.monmap->epoch ? 1:0;
		handle_monmap(client, msg);
		if (!had && client->monc.monmap->epoch)
			got_first_map(client, 0);
		break;

		/* mon client */
	case CEPH_MSG_STATFS_REPLY:
		ceph_monc_handle_statfs_reply(&client->monc, msg);
		break;

		/* mds client */
	case CEPH_MSG_MDS_MAP:
		had = client->mdsc.mdsmap ? 1:0;
		ceph_mdsc_handle_map(&client->mdsc, msg);
		if (!had && client->mdsc.mdsmap) 
			got_first_map(client, 1);
		break;
	case CEPH_MSG_CLIENT_SESSION:
		ceph_mdsc_handle_session(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_REPLY:
		ceph_mdsc_handle_reply(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_REQUEST_FORWARD:
		ceph_mdsc_handle_forward(&client->mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_FILECAPS:
		ceph_mdsc_handle_filecaps(&client->mdsc, msg);
		break;

		/* osd client */
	case CEPH_MSG_OSD_MAP:
		had = client->osdc.osdmap ? 1:0;
		ceph_osdc_handle_map(&client->osdc, msg);
		if (!had && client->osdc.osdmap) 
			got_first_map(client, 2);
		break;
	case CEPH_MSG_OSD_OPREPLY:
		ceph_osdc_handle_reply(&client->osdc, msg);
		break;

	default:
		derr(1, "dispatch unknown message type %d\n", msg->hdr.type);
	}

	ceph_msg_put(msg);
}
