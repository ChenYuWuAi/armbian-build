#!/usr/bin/env python3
"""Apply the elish ADD_TOPOLOGIES support to mainline q6adm.c (patch 0053)."""
import sys

p = '/home/axis/axis_rnd/work/kernel/build/linux-6.12.58/sound/soc/qcom/qdsp6/q6adm.c'
s = open(p).read()
orig = s
n = 0

def sub(anchor, addition, after=True, label=''):
    global s, n
    c = s.count(anchor)
    assert c == 1, 'anchor %r found %d times (%s)' % (anchor[:60], c, label)
    s = s.replace(anchor, anchor + addition if after else addition + anchor, 1)
    n += 1

# ---- 1) includes -------------------------------------------------------
sub('#include <linux/device.h>\n',
    '#include <linux/dma-mapping.h>\n#include <linux/firmware.h>\n',
    after=True, label='includes')

# ---- 2) defines + wire structs ----------------------------------------
a = '#define ADM_CMDRSP_DEVICE_OPEN_V5\t0x00010329\n'
sub(a, '''
/*
 * elish: send the ACDB "CORE_CUSTOM_TOPOLOGIES" blob to the ADSP so the real
 * speaker audproc COPP topologies (0x1000a100 / 0x1000a101) get instantiated.
 * mainline never sends these, so the DSP only ever knows NULL_COPP_TOPOLOGY.
 * ADD_TOPOLOGIES is out-of-band: the payload has to be mapped into the ADSP
 * (ADM_CMD_SHARED_MEM_MAP_REGIONS) before the command can reference it.
 */
#define ADM_CMD_SHARED_MEM_MAP_REGIONS\t\t0x00010322
#define ADM_CMDRSP_SHARED_MEM_MAP_REGIONS\t0x00010323
#define ADM_CMD_ADD_TOPOLOGIES\t\t\t0x00010335
#define ADM_ELISH_MAP_POOL\t\t\t3\t/* ADSP_MEMORY_MAP_SHMEM8_4K_POOL */

struct avs_cmd_shared_mem_map_regions {
\tu16 mem_pool_id;
\tu16 num_regions;
\tu32 property_flag;
} __packed;

struct avs_shared_map_region_payload {
\tu32 shm_addr_lsw;
\tu32 shm_addr_msw;
\tu32 mem_size_bytes;
} __packed;

struct cmd_set_topologies {
\tstruct apr_hdr hdr;
\tu32 payload_addr_lsw;
\tu32 payload_addr_msw;
\tu32 mem_map_handle;
\tu32 payload_size;
} __packed;
''', label='defines')

# ---- 3) struct q6adm fields -------------------------------------------
sub('\twait_queue_head_t matrix_map_wait;\n',
    '''\t/* elish: add-topologies (out-of-band) state */
\tstruct mutex topo_lock;
\twait_queue_head_t topo_wait;
\tstruct delayed_work topo_work;
\tvoid *topo_buf;
\tdma_addr_t topo_dma;
\tu32 topo_handle;
\tu32 topo_status;
\tbool topo_map_done;
\tbool topo_add_done;
''', label='struct fields')

# ---- 4) callback: basic-rsp case for ADD_TOPOLOGIES --------------------
sub('''\t\tdefault:
\t\t\tdev_err(&adev->dev, "Unknown Cmd: 0x%x\\n",
\t\t\t\tresult->opcode);
\t\t\tbreak;
''',
    '''\t\tcase ADM_CMD_ADD_TOPOLOGIES:
\t\t\tadm->topo_status = result->status;
\t\t\tadm->topo_add_done = true;
\t\t\twake_up(&adm->topo_wait);
\t\t\tbreak;
''', after=False, label='callback basic rsp')

# ---- 5) callback: map response -> mem_map_handle ----------------------
sub('\tcase ADM_CMDRSP_DEVICE_OPEN_V5: {\n',
    '''\tcase ADM_CMDRSP_SHARED_MEM_MAP_REGIONS:
\t\t/* elish: ADM returns the map handle as the first u32 of the
\t\t * response payload (note: ASM instead uses hdr->opcode). */
\t\tadm->topo_handle = *(u32 *)data->payload;
\t\tadm->topo_map_done = true;
\t\twake_up(&adm->topo_wait);
\t\treturn 0;

''', after=False, label='callback map rsp')

# ---- 6) the implementation, inserted before q6adm_probe ---------------
sub('static int q6adm_probe(struct apr_device *adev)\n',
    '''/**
 * q6adm_add_topologies() - map a topology blob and send ADM_CMD_ADD_TOPOLOGIES
 * @adm_dev: the ADM apr_device (dev_get_drvdata must be struct q6adm)
 * @fw:      topology definition blob (elish: 5444 bytes from the ACDB)
 * @len:     blob length
 *
 * ADD_TOPOLOGIES is out-of-band, so the blob is first copied into a
 * DMA-coherent buffer and mapped into the ADSP; the resulting handle is then
 * referenced by the command.
 */
int q6adm_add_topologies(struct device *adm_dev, const void *fw, size_t len)
{
\tstruct q6adm *adm = dev_get_drvdata(adm_dev);
\tstruct avs_cmd_shared_mem_map_regions *cmd;
\tstruct avs_shared_map_region_payload *mregions;
\tstruct cmd_set_topologies *topo;
\tstruct apr_pkt *pkt;
\tsize_t buf_sz;
\tint pkt_size, rc = 0;
\tvoid *p;

\tif (!adm || !fw || !len)
\t\treturn -EINVAL;

\tmutex_lock(&adm->topo_lock);

\tbuf_sz = ALIGN(len, 4096);
\tadm->topo_buf = dma_alloc_coherent(adm->dev, buf_sz, &adm->topo_dma,
\t\t\t\t\t   GFP_KERNEL);
\tif (!adm->topo_buf) {
\t\trc = -ENOMEM;
\t\tgoto out;
\t}
\tmemcpy(adm->topo_buf, fw, len);

\t/* 1) map the payload buffer into the ADSP */
\tpkt_size = APR_HDR_SIZE + sizeof(*cmd) + sizeof(*mregions);
\tp = kzalloc(pkt_size, GFP_KERNEL);
\tif (!p) {
\t\trc = -ENOMEM;
\t\tgoto out;
\t}
\tpkt = p;
\tcmd = p + APR_HDR_SIZE;
\tmregions = p + APR_HDR_SIZE + sizeof(*cmd);

\tpkt->hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
\tpkt->hdr.pkt_size = pkt_size;
\tpkt->hdr.src_port = 0;
\tpkt->hdr.dest_port = 0;
\tpkt->hdr.token = 0;
\tpkt->hdr.opcode = ADM_CMD_SHARED_MEM_MAP_REGIONS;

\tcmd->mem_pool_id = ADM_ELISH_MAP_POOL;
\tcmd->num_regions = 1;
\tcmd->property_flag = 0;

\tmregions->shm_addr_lsw = lower_32_bits(adm->topo_dma);
\tmregions->shm_addr_msw = upper_32_bits(adm->topo_dma);
\tmregions->mem_size_bytes = buf_sz;

\tadm->topo_map_done = false;
\tadm->topo_handle = 0;
\trc = apr_send_pkt(adm->apr, pkt);
\tkfree(p);
\tif (rc < 0) {
\t\tdev_err(adm->dev, "elish: map regions send failed %d\\n", rc);
\t\tgoto out;
\t}
\tif (!wait_event_timeout(adm->topo_wait, adm->topo_map_done, 5 * HZ)) {
\t\tdev_err(adm->dev, "elish: map regions timeout\\n");
\t\trc = -ETIMEDOUT;
\t\tgoto out;
\t}
\tdev_info(adm->dev, "elish: topologies mapped (handle=0x%x, %zu bytes)\\n",
\t\t adm->topo_handle, buf_sz);

\t/* 2) add the topologies */
\ttopo = kzalloc(sizeof(*topo), GFP_KERNEL);
\tif (!topo) {
\t\trc = -ENOMEM;
\t\tgoto out;
\t}
\ttopo->hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
\ttopo->hdr.pkt_size = sizeof(*topo);
\ttopo->hdr.src_port = 0;
\ttopo->hdr.dest_port = 0;
\ttopo->hdr.token = 0;
\ttopo->hdr.opcode = ADM_CMD_ADD_TOPOLOGIES;
\ttopo->payload_addr_lsw = lower_32_bits(adm->topo_dma);
\ttopo->payload_addr_msw = upper_32_bits(adm->topo_dma);
\ttopo->mem_map_handle = adm->topo_handle;
\ttopo->payload_size = len;

\tadm->topo_add_done = false;
\tadm->topo_status = 0;
\trc = apr_send_pkt(adm->apr, (struct apr_pkt *)topo);
\tkfree(topo);
\tif (rc < 0) {
\t\tdev_err(adm->dev, "elish: add topologies send failed %d\\n", rc);
\t\tgoto out;
\t}
\tif (!wait_event_timeout(adm->topo_wait, adm->topo_add_done, 5 * HZ)) {
\t\tdev_err(adm->dev, "elish: add topologies timeout\\n");
\t\trc = -ETIMEDOUT;
\t\tgoto out;
\t}
\tif (adm->topo_status) {
\t\tdev_err(adm->dev, "elish: DSP rejected ADD_TOPOLOGIES, status=0x%x\\n",
\t\t\tadm->topo_status);
\t\trc = -EINVAL;
\t\tgoto out;
\t}
\tdev_info(adm->dev, "elish: ADD_TOPOLOGIES ok (%zu bytes)\\n", len);

out:
\tmutex_unlock(&adm->topo_lock);
\treturn rc;
}
EXPORT_SYMBOL_GPL(q6adm_add_topologies);

/* elish: at ADM probe, push /lib/firmware/elish_topologies.bin if present.
 * Absent file is normal on every other board. */
static void q6adm_topo_work(struct work_struct *work)
{
\tstruct q6adm *adm = container_of(work, struct q6adm, topo_work.work);
\tconst struct firmware *fw;
\tint rc;

\trc = request_firmware(&fw, "elish_topologies.bin", adm->dev);
\tif (rc) {
\t\tdev_info(adm->dev, "elish: no elish_topologies.bin (%d), skipping\\n",
\t\t\t rc);
\t\treturn;
\t}
\trc = q6adm_add_topologies(adm->dev, fw->data, fw->size);
\trelease_firmware(fw);
\tif (rc)
\t\tdev_err(adm->dev, "elish: add topologies failed %d\\n", rc);
}

static int q6adm_probe(struct apr_device *adev)
''', after=False, label='implementation')

# ---- 7) probe init + schedule -----------------------------------------
sub('\tinit_waitqueue_head(&adm->matrix_map_wait);\n',
    '''\tmutex_init(&adm->topo_lock);
\tinit_waitqueue_head(&adm->topo_wait);
\tINIT_DELAYED_WORK(&adm->topo_work, q6adm_topo_work);
\tschedule_delayed_work(&adm->topo_work, msecs_to_jiffies(5000));
''', label='probe init')

open(p, 'w').write(s)
print('applied %d edits, file grew by %d bytes' % (n, len(s) - len(orig)))
