#!/usr/bin/env python3
"""Add the AFE-side add-topologies path to mainline q6afe.c.

Anchors verified against the real file (2026-09-22):
  includes   : '#include <linux/delay.h>'        (q6afe.c has NO linux/device.h)
  defines    : '#include <linux/soc/qcom/apr.h>' (must be after apr.h)
  fields     : '\tspinlock_t port_list_lock;'
  probe hook : '\tinit_waitqueue_head(&afe->wait);'
  impl       : before 'static int q6afe_callback('
All anchors are asserted one by one; the file is written only at the very end,
so an aborted run leaves q6afe.c untouched.
"""
P = '/home/axis/axis_rnd/work/kernel/build/linux-6.12.58/sound/soc/qcom/qdsp6/q6afe.c'
s = open(P).read()
n = 0


def sub(anchor, add, after=True, label=''):
    global s, n
    c = s.count(anchor)
    assert c == 1, '%s: anchor found %d times' % (label, c)
    s = s.replace(anchor, anchor + add if after else add + anchor, 1)
    n += 1


sub('#include <linux/delay.h>\n',
    '#include <linux/dma-mapping.h>\n#include <linux/firmware.h>\n', label='includes')

sub('#include <linux/soc/qcom/apr.h>\n', '''
/*
 * elish: AFE-side add-topologies.  The ACDB "common custom topology" blob
 * (5444 bytes) defines the real speaker audproc topologies (0x1000a100 /
 * 0x1000a101).  ADM rejects it with EINVAL (it only registers
 * ADM_CUST_TOPOLOGY cal type 10), so push it through the AFE service, which
 * has its own AFE_CMD_ADD_TOPOLOGIES.
 */
#define AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS\t\t0x000100EA
#define AFE_SERVICE_CMDRSP_SHARED_MEM_MAP_REGIONS\t0x000100EB
#define AFE_CMD_ADD_TOPOLOGIES\t\t\t\t0x000100f8
#define ELISH_AFE_MAP_POOL\t\t\t\t3

struct elish_map_regions_cmd {
\tu16 mem_pool_id;
\tu16 num_regions;
\tu32 property_flag;
} __packed;

struct elish_map_region {
\tu32 shm_addr_lsw;
\tu32 shm_addr_msw;
\tu32 mem_size_bytes;
} __packed;

struct elish_cmd_set_topologies {
\tstruct apr_hdr hdr;
\tu32 payload_addr_lsw;
\tu32 payload_addr_msw;
\tu32 mem_map_handle;
\tu32 payload_size;
} __packed;
''', label='defines')

sub('\tspinlock_t port_list_lock;\n', '''\t/* elish: AFE add-topologies state */
\tstruct delayed_work elish_topo_work;
\twait_queue_head_t elish_topo_wait;
\tvoid *elish_topo_buf;
\tdma_addr_t elish_topo_dma;
\tu32 elish_topo_handle;
\tu32 elish_topo_status;
\tbool elish_topo_map_done;
\tbool elish_topo_add_done;
''', label='fields')

IMPL = '''/* elish: opt-in, default OFF.  A bad payload makes the ADSP take a fatal
 * error it cannot recover from, which kills audio until reboot. */
static bool elish_topologies;
module_param(elish_topologies, bool, 0644);
MODULE_PARM_DESC(elish_topologies,
\t"elish: push /lib/firmware/elish_topologies.bin via AFE (default off)");

static int elish_afe_add_topologies(struct q6afe *afe, const void *fw, size_t len)
{
\tstruct elish_map_regions_cmd *cmd;
\tstruct elish_map_region *mregions;
\tstruct elish_cmd_set_topologies *topo;
\tstruct apr_pkt *pkt;
\tsize_t buf_sz;
\tstruct device *d;
\tint pkt_size, rc, lvl;
\tvoid *p;

\tbuf_sz = ALIGN(len, 4096);
\t/* The ADSP faults on buffers above 4GB: walk the device chain until a
\t * device whose DMA ops can give a low address answers. */
\td = afe->dev;
\tfor (lvl = 0; d && lvl < 6 && !afe->elish_topo_buf; lvl++) {
\t\tafe->elish_topo_buf = dma_alloc_coherent(d, buf_sz,
\t\t\t\t\t\t\t &afe->elish_topo_dma,
\t\t\t\t\t\t\t GFP_KERNEL);
\t\td = d->parent;
\t}
\tif (!afe->elish_topo_buf) {
\t\tafe->elish_topo_buf = kzalloc(buf_sz, GFP_KERNEL);
\t\tif (afe->elish_topo_buf)
\t\t\tafe->elish_topo_dma = virt_to_phys(afe->elish_topo_buf);
\t}
\tif (!afe->elish_topo_buf)
\t\treturn -ENOMEM;
\tif (afe->elish_topo_dma >> 32) {
\t\tdev_err(afe->dev, "elish: afe dma=0x%llx above 4GB, refusing\\n",
\t\t\t(unsigned long long)afe->elish_topo_dma);
\t\treturn -ERANGE;
\t}
\tdev_info(afe->dev, "elish: afe buffer %zu dma=0x%llx\\n", buf_sz,
\t\t (unsigned long long)afe->elish_topo_dma);
\tmemcpy(afe->elish_topo_buf, fw, len);

\t/* 1) map the buffer into the ADSP through the AFE service */
\tpkt_size = APR_HDR_SIZE + sizeof(*cmd) + sizeof(*mregions);
\tp = kzalloc(pkt_size, GFP_KERNEL);
\tif (!p)
\t\treturn -ENOMEM;
\tpkt = p;
\tcmd = p + APR_HDR_SIZE;
\tmregions = p + APR_HDR_SIZE + sizeof(*cmd);
\tpkt->hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
\tpkt->hdr.pkt_size = pkt_size;
\tpkt->hdr.opcode = AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS;
\tcmd->mem_pool_id = ELISH_AFE_MAP_POOL;
\tcmd->num_regions = 1;
\tmregions->shm_addr_lsw = lower_32_bits(afe->elish_topo_dma);
\tmregions->shm_addr_msw = upper_32_bits(afe->elish_topo_dma);
\tmregions->mem_size_bytes = buf_sz;
\tafe->elish_topo_map_done = false;
\tafe->elish_topo_handle = 0;
\trc = apr_send_pkt(afe->apr, pkt);
\tkfree(p);
\tif (rc < 0)
\t\treturn rc;
\tif (!wait_event_timeout(afe->elish_topo_wait, afe->elish_topo_map_done,
\t\t\t\t5 * HZ)) {
\t\tdev_err(afe->dev, "elish: afe map timeout\\n");
\t\treturn -ETIMEDOUT;
\t}
\tdev_info(afe->dev, "elish: afe mapped handle=0x%x\\n", afe->elish_topo_handle);

\t/* 2) add the topologies */
\ttopo = kzalloc(sizeof(*topo), GFP_KERNEL);
\tif (!topo)
\t\treturn -ENOMEM;
\ttopo->hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
\ttopo->hdr.pkt_size = sizeof(*topo);
\ttopo->hdr.opcode = AFE_CMD_ADD_TOPOLOGIES;
\ttopo->payload_addr_lsw = lower_32_bits(afe->elish_topo_dma);
\ttopo->payload_addr_msw = upper_32_bits(afe->elish_topo_dma);
\ttopo->mem_map_handle = afe->elish_topo_handle;
\ttopo->payload_size = len;
\tafe->elish_topo_add_done = false;
\tafe->elish_topo_status = 0;
\trc = apr_send_pkt(afe->apr, (struct apr_pkt *)topo);
\tkfree(topo);
\tif (rc < 0)
\t\treturn rc;
\tif (!wait_event_timeout(afe->elish_topo_wait, afe->elish_topo_add_done,
\t\t\t\t5 * HZ)) {
\t\tdev_err(afe->dev, "elish: afe add topologies timeout\\n");
\t\treturn -ETIMEDOUT;
\t}
\tif (afe->elish_topo_status) {
\t\tdev_err(afe->dev, "elish: AFE rejected ADD_TOPOLOGIES status=0x%x\\n",
\t\t\tafe->elish_topo_status);
\t\treturn -EINVAL;
\t}
\tdev_info(afe->dev, "elish: AFE ADD_TOPOLOGIES ok (%zu bytes)\\n", len);
\treturn 0;
}

static void elish_afe_topo_work(struct work_struct *work)
{
\tstruct q6afe *afe = container_of(work, struct q6afe, elish_topo_work.work);
\tconst struct firmware *fw;
\tint rc, tries;

\tfor (tries = 0; tries < 150 && !elish_topologies; tries++)
\t\tmsleep(2000);
\tif (!elish_topologies) {
\t\tdev_info(afe->dev, "elish: afe topologies disabled\\n");
\t\treturn;
\t}
\trc = request_firmware(&fw, "elish_topologies.bin", afe->dev);
\tif (rc) {
\t\tdev_info(afe->dev, "elish: no elish_topologies.bin (%d)\\n", rc);
\t\treturn;
\t}
\trc = elish_afe_add_topologies(afe, fw->data, fw->size);
\trelease_firmware(fw);
\tif (rc)
\t\tdev_err(afe->dev, "elish: afe add topologies failed %d\\n", rc);
}

'''
sub('static int q6afe_callback(struct apr_device *adev, struct apr_resp_pkt *data)\n',
    IMPL, after=False, label='impl')

# callback: self-contained handling at the top of the body
cb = 'static int q6afe_callback(struct apr_device *adev, struct apr_resp_pkt *data)\n{\n'
sub(cb, '''\tstruct q6afe *elish_afe = dev_get_drvdata(&adev->dev);

\tif (data->payload_size >= 4 &&
\t    data->hdr.opcode == AFE_SERVICE_CMDRSP_SHARED_MEM_MAP_REGIONS) {
\t\telish_afe->elish_topo_handle = *(u32 *)data->payload;
\t\telish_afe->elish_topo_map_done = true;
\t\twake_up(&elish_afe->elish_topo_wait);
\t\treturn 0;
\t}
\tif (data->hdr.opcode == APR_BASIC_RSP_RESULT && data->payload_size >= 8) {
\t\tstruct aprv2_ibasic_rsp_result_t *r = data->payload;

\t\tif (r->opcode == AFE_CMD_ADD_TOPOLOGIES) {
\t\t\telish_afe->elish_topo_status = r->status;
\t\t\telish_afe->elish_topo_add_done = true;
\t\t\twake_up(&elish_afe->elish_topo_wait);
\t\t\treturn 0;
\t\t}
\t}
''', label='callback')

sub('\tinit_waitqueue_head(&afe->wait);\n', '''\tinit_waitqueue_head(&afe->elish_topo_wait);
\tINIT_DELAYED_WORK(&afe->elish_topo_work, elish_afe_topo_work);
\tschedule_delayed_work(&afe->elish_topo_work, msecs_to_jiffies(8000));
''', label='probe')

open(P, 'w').write(s)
print('q6afe.c patched: %d edits, now %d lines' % (n, s.count(chr(10))))
