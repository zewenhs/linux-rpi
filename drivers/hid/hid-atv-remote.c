/*
 *  HID driver for the Android TV remote
 *  providing keys and microphone audio functionality
 *
 * Copyright (C) 2014 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/hid.h>
#include <linux/hiddev.h>
#include <linux/hardirq.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#ifdef WITH_SWITCH
#include <linux/switch.h>
#endif
#include <sound/core.h>
#include <sound/control.h>
#include <sound/info.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <linux/version.h>

#include "hid-ids.h"
#include "sbcdec.h"


MODULE_LICENSE("GPL v2");

//#define EACH_WITH_HEAD_MODE // Every notification with 2B ADPCM
// Define this if you wanna use the fake wav data, before get the pcm data from 
// snd, it's needed to operate the voice RC(eg: Up,Down) to make it start the timer
//#define TEST_ADPCM_DATA_FILL
// Generate a noise every seconds for the sine wave test case
//#define FAKE_DATA_GEN_NOISE
// Generate a Notification lossing, every seconds for the sine wave test case
//#define FAKE_LOST_A_NOTIFICATION
//#define R_32K_DATA

#define snd_atvr_log(...) pr_info("snd_atvr: " __VA_ARGS__)

#ifdef TEST_ADPCM_DATA_FILL
//#include "wav_file1.h"
//#include "wav_file.h"
//#include "wav_file2.h"
//#include "wav_file3.h"
//#include "wav_file4.h"
//include "wav_file5.h"
//#include "wav_file6.h"
//#include "wav_file7.h"
#include "wav_file13.h"
//#include "wav_file12.h"
#endif 

int rev_packet_num = 0;
u8 audio_rev_buf[126];

/* These values are copied from Android WiredAccessoryObserver */
enum headset_state {
	BIT_NO_HEADSET = 0,
	BIT_HEADSET = (1 << 0),
	BIT_HEADSET_NO_MIC = (1 << 1),
};

static struct snd_card *pcard = NULL;

/* This has to be static and created at init/boot, or else Android
 * WiredAccessoryManager won't watch for it since it only checks
 * for it's existence once on boot.
 */
#ifdef WITH_SWITCH
static struct switch_dev h2w_switch = {
	.name = "h2w",
};
#endif

#define ADPCM_AUDIO_REPORT_ID 30

#define MSBC_AUDIO1_REPORT_ID 0xF7
#define MSBC_AUDIO2_REPORT_ID 0xFA
#define MSBC_AUDIO3_REPORT_ID 0xFB

#define INPUT_REPORT_ID 2

#define KEYCODE_PRESENT_IN_AUDIO_PACKET_FLAG 0x80

/* defaults */
#define MAX_PCM_DEVICES     1
#define MAX_PCM_SUBSTREAMS  4
#define MAX_MIDI_DEVICES    0

/* Define these all in one place so they stay in sync. */

unsigned int USE_RATE_MIN = 8000;
unsigned int USE_RATE_MAX = 8000;
//unsigned int USE_RATES_ARRAY = USE_RATE_MIN;
unsigned int USE_RATES_MASK = SNDRV_PCM_RATE_8000;

size_t MAX_FRAMES_PER_BUFFER = 8192;

unsigned int USE_PERIODS_MAX = 1024; // Ori 1024

size_t MAX_PCM_BUFFER_SIZE;
size_t MIN_PERIOD_SIZE = 64; //Ori64
size_t MAX_PERIOD_SIZE;

#define MAX_SAMPLES_PER_PACKET  128
size_t MIN_SAMPLES_PER_PACKET_P2 = 32;
size_t MAX_PACKETS_PER_BUFFER;
size_t MAX_BUFFER_SIZE;

static short mic_sample_rate = 16;
module_param_named(rate, mic_sample_rate, short, S_IRUGO);
MODULE_PARM_DESC(mic_sample_rate, "mic sample rate");

static int dev_id[4] = {0xBBBB, 0xBBBB, 0xBBBB, 0xBBBB};
static int n_dev_id = 4;
module_param_array(dev_id, int, &n_dev_id, S_IRUGO);
MODULE_PARM_DESC(dev_id, "HID device id for compaired BLE device.");

static  struct hid_device_id g_atvr_devices[];

#define USE_CHANNELS_MIN   1
#define USE_CHANNELS_MAX   1
#define USE_PERIODS_MIN    1

#define USE_FORMATS          (SNDRV_PCM_FMTBIT_S16_LE)

#define PACKET_TYPE_ADPCM 0
#define PACKET_TYPE_MSBC  1

#ifdef TEST_ADPCM_DATA_FILL

void mic_to_adpcm_split (signed short *ps, int len, signed short *pds, int start);
static void audio_dec(const uint8_t *raw_input, int type, size_t num_bytes);
//extern const unsigned char sinWave_PCM_1K_3S[96010];
//extern const unsigned char hexData[32008];
//extern const unsigned char hexData_10s[320008];
const unsigned char *pDataBuffer = hexData_10s;

#ifndef EACH_WITH_HEAD_MODE
#define NOTIFICATION_LENGTH 20
#else 
//#define NOTIFICATION_LENGTH 20
#define NOTIFICATION_LENGTH 126
//#define NOTIFICATION_LENGTH 120
//#define NOTIFICATION_LENGTH 140
//#define NOTIFICATION_LENGTH 60
#endif

#define FAKE_NOTIFI_NUMS 100
#define ADPCM_RATIO 4
#define BYTES_PER_SAMPLE 2
#define SAMPLES_PER_NOTIFICATION (ADPCM_RATIO*NOTIFICATION_LENGTH/BYTES_PER_SAMPLE) // 4*20/2=40
#ifndef R_32K_DATA
unsigned int g_time_interval = 2500; // 1Notification = 20BADPCM = 80Byte PCM = 2.5ms(80/2/16)
//unsigned int g_time_interval =  25000; // 1Notification = 20BADPCM = 80Byte PCM = 2.5ms(80/2/16)
//unsigned int g_time_interval =  12000; // 1Notification = 20BADPCM = 80Byte PCM = 2.5ms(80/2/16)
#else 
//unsigned int g_time_interval = 2000; // 1Notification = 20BADPCM = 80Byte PCM = 2.5ms(80/2/16)
unsigned int g_time_interval =  2500; // 1Notification = 20BADPCM = 80Byte PCM = 2.5ms(80/2/16)
#endif
struct timer_list g_timer;
static unsigned int fake_notifiy_index = 0;

//unsigned char fake_data[NOTIFICATION_LENGTH] = {};
//unsigned char fakeData_FirstHeaderData[NOTIFICATION_LENGTH * FAKE_NOTIFI_NUM] = {};
//signed short ps[40]; // Store the uncompressed ACM notification data: 2*40=80B
//signed short pds[10]; // Notification data: 2*10=20B

//int16_t ps[80]; // Store the uncompressed ACM notification data: 2*40=80B
//int16_t ps[498]; // Store the uncompressed ACM notification data: 2*40=80B
int16_t ps[80]; // Store the uncompressed ACM notification data: 2*40=80B
//int16_t pds[10]; // Notification data: 2*10=20B
int16_t pds[10]; // Notification data: 2*10=20B
static unsigned int test_counter = 0;
 
void _TimerHandler(unsigned long data)
{
    int each = 0;
    test_counter ++;
    /*Restarting the timer...*/
   // printk(KERN_INFO "before Mod count=%ul\n", test_counter);
    mod_timer( &g_timer, jiffies + usecs_to_jiffies(g_time_interval));
    //unsigned char *pFakeData = NULL;
    fake_notifiy_index ++;
//if(fake_notifiy_index == 1 || fake_notifiy_index == 3 || fake_notifiy_index ==100)
	//printk("miles---> [FUNC]:%s [LINE]:%d fake_notifiy_index:%d\n",  __FUNCTION__, __LINE__, fake_notifiy_index);
#ifdef EACH_WITH_HEAD_MODE // Every notification with 2B ADPCM
    each = 1;
#endif
    if ( fake_notifiy_index == 1) {
        //memcpy(ps, pDataBuffer, 80);
#ifdef R_32K_DATA
        //memcpy(ps, pDataBuffer, 148);
        memcpy(ps, pDataBuffer, 160);
#else
        memcpy(ps, pDataBuffer, 80);
       // memcpy(ps, pDataBuffer, 498);
       // memcpy(ps, pDataBuffer, 474);
        //memcpy(ps, pDataBuffer, 554);
       // memcpy(ps, pDataBuffer, 234);
#endif
        //mic_to_adpcm_split (signed short ps, int len, signed short pds, int start)
        mic_to_adpcm_split (ps, 40, pds, 1);
        //mic_to_adpcm_split (ps, 249, pds, 1);
       // mic_to_adpcm_split (ps, 237, pds, 1);
        //mic_to_adpcm_split (ps, 277, pds, 1);
        //mic_to_adpcm_split (ps, 117, pds, 1);
        
		//pFakeData = fakeData_FirstHeaderData;
    } else {
        //memcpy(ps, pDataBuffer+(fake_notifiy_index-1)*74, 74);
#ifdef FAKE_DATA_GEN_NOISE
		dgeereeeee
        // Generate a noise every 1Seconds
        if(test_counter % 500 == 0) {
            if(ps[2] > 0) {
                ps[2] = -32000;
            } else {
                ps[2] = 32000;
            }
            //ps[3] = -ps[3];
            ps[4] = -ps[4];
            //ps[5] = -ps[5];
            ps[6] = -ps[6];
            //ps[7] = -ps[7];
            ps[8] = -ps[8];
            //ps[9] = -ps[9];
            printk(KERN_INFO "Noise generated, ori value=%d\n", ps[2]);
        }
#endif

#ifndef EACH_WITH_HEAD_MODE // Every notification with 2B ADPCM
	#ifndef R_32K_DATA 
        memcpy(ps, pDataBuffer+(fake_notifiy_index-1)*80, 80);
	#else 
        memcpy(ps, pDataBuffer+(fake_notifiy_index-1)*160, 160);
	#endif
        mic_to_adpcm_split (ps, 40, pds, 0);
#else
	#ifdef R_32K_DATA
        //memcpy(ps, pDataBuffer+(fake_notifiy_index-1)*148, 148);
        memcpy(ps, pDataBuffer+(fake_notifiy_index-1)*996, 996);
	#else
        //memcpy(ps, pDataBuffer+(fake_notifiy_index-1)*80, 80);
        memcpy(ps, pDataBuffer+(fake_notifiy_index-1)*498, 498);
        //memcpy(ps, pDataBuffer+(fake_notifiy_index-1)*474, 474);
        //memcpy(ps, pDataBuffer+(fake_notifiy_index-1)*554, 554);
        //memcpy(ps, pDataBuffer+(fake_notifiy_index-1)*234, 234);
	#endif
        //mic_to_adpcm_split (ps, 36, pds, 1);
        //mic_to_adpcm_split (ps, 249, pds, 1);
        mic_to_adpcm_split (ps, 237, pds, 1);
        //mic_to_adpcm_split (ps, 277, pds, 1);
        //mic_to_adpcm_split (ps, 117, pds, 1);
#endif

#ifdef FAKE_LOST_A_NOTIFICATION
		ddeeees
        // Generate a noise every 1Seconds
        if(test_counter % 500 == 0) {
            if(ps[2] > 0) {
                ps[2] = -32000;
            } else {
                ps[2] = 32000;
            }
            //ps[3] = -ps[3];
            ps[4] = -ps[4];
            //ps[5] = -ps[5];
            ps[6] = -ps[6];
            //ps[7] = -ps[7];
            ps[8] = -ps[8];
            //ps[9] = -ps[9];
            printk(KERN_INFO "Pretend a notification lossed\n");
            memset(pds, 0, sizeof(pds));
        }
#endif
        //pFakeData = fakeData[fake_notifiy_index * NOTIFICATION_LENGTH];
    }
	unsigned char *ppp = (unsigned char *)pds;
	int ii;
	static int ip = 0;
#if 0//local adpcm, zewen
	if(ip < 500)
	{
for(ii = 0; ii<NOTIFICATION_LENGTH;ii++)
	printk("%x ", ppp[ii]);
printk("\n");
ip++;
	}
#endif
    audio_dec((unsigned char*) pds, PACKET_TYPE_ADPCM, NOTIFICATION_LENGTH); // Notification length

    
   // printk(KERN_INFO "Handler.\n");
    //if(80*fake_notifiy_index + 160 > sizeof(sinWave_PCM_1K_3S)){
    //if(80*(1+fake_notifiy_index)  == 320000){
#ifndef EACH_WITH_HEAD_MODE
	#ifndef R_32K_DATA
    	if(80*(fake_notifiy_index)  >= sizeof(hexData_10s)){
	#else
    	if(160*(fake_notifiy_index)  >= sizeof(hexData_10s)){
	#endif
#else
	#ifdef R_32K_DATA
	//if(74*(fake_notifiy_index)  > sizeof(hexData_10s)){
	if(996*(fake_notifiy_index)  > sizeof(hexData_10s)){
	#else
	//if(74*(fake_notifiy_index)  > sizeof(hexData_10s)){
	//if(498*(fake_notifiy_index)  > sizeof(hexData_10s)){
	if(474*(fake_notifiy_index)  >= sizeof(hexData_10s)){
	//if(554*(fake_notifiy_index)  >= sizeof(hexData_10s)){
	//if(234*(fake_notifiy_index)  > sizeof(hexData_10s)){
	#endif
#endif
       fake_notifiy_index = 200/80;
     //  fake_notifiy_index = 2;
    }
    if(fake_notifiy_index >= 320000) {
        printk(KERN_ERR "fake index overflow!!!\n");
    }
}

static void filldata_test_timer_start(void)
{
    /*Starting the test timer.*/
    setup_timer(&g_timer, _TimerHandler, 0);
    mod_timer( &g_timer, jiffies + usecs_to_jiffies(g_time_interval));
}

static void stop_test_timer(void)
{
	int ret;
	if (!in_interrupt()) {
		/* del_timer_sync will hang if called in the timer callback. */
		ret = del_timer_sync(&g_timer);
		if (ret < 0)
			pr_err("%s:%d - ERROR del test timer_sync failed, %d\n", __func__, __LINE__, ret);
	}
}
#endif

/* Normally SBC has a H2 header but because we want
 * to embed keycode support while audio is active without
 * incuring an additional packet in the connection interval,
 * we only use half the H2 header.  A normal H2 header has
 * a 12-bit synchronization word and a 2-bit sequence number
 * (SN0, SN1).  The sequence number is duplicated, so each
 * pair of bits in the sequence number shall be always 00
 * or 11 (see 5.7.2 of HFP_SPEC_V16).  We only receive
 * the second byte of the H2 header that has the latter part
 * of the sync word and the entire sequence number.
 *
 *  0      70      7
 * b100000000001XXYY - where X is SN0 repeated and Y is SN1 repeated
 *
 * So the sequence numbers are:
 * b1000000000010000 - 0x01 0x08  - only the 0x08 is received
 * b1000000000011100 - 0x01 0x38  - only the 0x38 is received
 * b1000000000010011 - 0x01 0xc8  - only the 0xc8 is received
 * b1000000000011111 - 0x01 0xf8  - only the 0xf8 is received
 *
 * Each mSBC frame is split over 3 BLE frames, where each BLE packet has
 * a 20 byte payload.
 * The first BLE packet has the format:
 * byte 0: keycode LSB
 * byte 1: keycode MSB, with most significant bit 0 for no key
 *         code active and 1 if keycode is active
 * byte 2: Second byte of H2
 * bytes 3-19: then four byte SBC header, then 13 bytes of audio data
 *
 * The second and third packet are purely 20 bytes of audio
 * data.  Second packet arrives on report 0xFA and third packet
 * arrives on report 0xFB.
 *
 * The mSBC decoder works on a mSBC frame, including the four byte SBC header,
 * so we have to accumulate 3 BLE packets before sending it to the decoder.
 */
#define NUM_SEQUENCES 4
const uint8_t mSBC_sequence_table[NUM_SEQUENCES] = {0x08, 0x38, 0xc8, 0xf8};
#define BLE_PACKETS_PER_MSBC_FRAME 3
#define MSBC_PACKET1_BYTES 17
#define MSBC_PACKET2_BYTES 20
#define MSBC_PACKET3_BYTES 20

#define BYTES_PER_MSBC_FRAME \
      (MSBC_PACKET1_BYTES + MSBC_PACKET2_BYTES + MSBC_PACKET3_BYTES)

const uint8_t mSBC_start_offset_in_packet[BLE_PACKETS_PER_MSBC_FRAME] = {
	1, /* SBC header starts after 1 byte sequence num portion of H2 */
	0,
	0
};
const uint8_t mSBC_start_offset_in_buffer[BLE_PACKETS_PER_MSBC_FRAME] = {
	0,
	MSBC_PACKET1_BYTES,
	MSBC_PACKET1_BYTES + MSBC_PACKET2_BYTES
};
const uint8_t mSBC_bytes_in_packet[BLE_PACKETS_PER_MSBC_FRAME] = {
	MSBC_PACKET1_BYTES, /* includes the SBC header but not the sequence num or keycode */
	MSBC_PACKET2_BYTES,
	MSBC_PACKET3_BYTES
};

struct fifo_packet {
	uint8_t  type;
	uint8_t  num_bytes;
	/* Expect no more than 20 bytes. But align struct size to power of 2. */
	//uint8_t  raw_data[30];
	uint8_t  raw_data[130];
};

//size_t MAX_SAMPLES_PER_PACKET = 128;
//size_t MIN_SAMPLES_PER_PACKET_P2 = 32;
//size_t MAX_PACKETS_PER_BUFFER = (MAX_FRAMES_PER_BUFFER / MIN_SAMPLES_PER_PACKET_P2);
//size_t MAX_BUFFER_SIZE = (MAX_PACKETS_PER_BUFFER * sizeof(struct fifo_packet));

#define SND_ATVR_RUNNING_TIMEOUT_MSEC    (500)


#define TIMER_STATE_BEFORE_DECODE    0
#define TIMER_STATE_DURING_DECODE    1
#define TIMER_STATE_AFTER_DECODE     2

static int packet_counter;
static int num_remotes;
static bool card_created = false;
static int dev;
static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;  /* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;   /* ID for this card */
static bool enable[SNDRV_CARDS] = {true, false};
/* Linux does not like NULL initialization. */
static char *model[SNDRV_CARDS]; /* = {[0 ... (SNDRV_CARDS - 1)] = NULL}; */
static int pcm_devs[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 1};
static int pcm_substreams[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = 1};

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for AndroidTV Remote soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for AndroidTV Remote soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable this AndroidTV Remote soundcard.");
module_param_array(model, charp, NULL, 0444);
MODULE_PARM_DESC(model, "Soundcard model.");
module_param_array(pcm_devs, int, NULL, 0444);
MODULE_PARM_DESC(pcm_devs, "PCM devices # (0-4) for AndroidTV Remote driver.");
module_param_array(pcm_substreams, int, NULL, 0444);
MODULE_PARM_DESC(pcm_substreams,
	"PCM substreams # (1-128) for AndroidTV Remote driver?");

/* Debug feature to save captured raw and decoded audio into buffers
 * and make them available for reading from misc devices.
 * It will record the last session only and only up to the buffer size.
 * The recording is cleared on read.
 */
#define DEBUG_WITH_MISC_DEVICE 1

/* Debug feature to trace audio packets being received */
#define DEBUG_AUDIO_RECEPTION 1

/* Debug feature to trace HID reports we see */
#define DEBUG_HID_RAW_INPUT 1

#if (DEBUG_WITH_MISC_DEVICE == 1)
static int16_t large_pcm_buffer[1280*1024];
static int large_pcm_index;

static struct miscdevice pcm_dev_node;
static int pcm_dev_open(struct inode *inode, struct file *file)
{
	/* nothing special to do here right now. */
	return 0;
}

static ssize_t pcm_dev_read(struct file *file, char __user *buffer,
			    size_t count, loff_t *ppos)
{
	const uint8_t *data = (const uint8_t *)large_pcm_buffer;
	size_t bytes_left = large_pcm_index * sizeof(int16_t) - *ppos;
	if (count > bytes_left)
		count = bytes_left;
	if (copy_to_user(buffer, &data[*ppos], count))
		return -EFAULT;

	*ppos += count;
	return count;
}

static const struct file_operations pcm_fops = {
	.owner = THIS_MODULE,
	.open = pcm_dev_open,
	.llseek = no_llseek,
	.read = pcm_dev_read,
};

static uint8_t raw_adpcm_buffer[640*1024];
static int raw_adpcm_index;
static struct miscdevice adpcm_dev_node;
static int adpcm_dev_open(struct inode *inode, struct file *file)
{
	/* nothing special to do here right now. */
	return 0;
}

static ssize_t adpcm_dev_read(struct file *file, char __user *buffer,
			  size_t count, loff_t *ppos)
{
	size_t bytes_left = raw_adpcm_index - *ppos;
	if (count > bytes_left)
		count = bytes_left;
	if (copy_to_user(buffer, &raw_adpcm_buffer[*ppos], count))
		return -EFAULT;

	*ppos += count;
	return count;
}

static const struct file_operations adpcm_fops = {
	.owner = THIS_MODULE,
	.open = adpcm_dev_open,
	.llseek = no_llseek,
	.read = adpcm_dev_read,
};

static uint8_t raw_mSBC_buffer[640*1024];
static int raw_mSBC_index;
static struct miscdevice mSBC_dev_node;
static int mSBC_dev_open(struct inode *inode, struct file *file)
{
	/* nothing special to do here right now. */
	return 0;
}

static ssize_t mSBC_dev_read(struct file *file, char __user *buffer,
			  size_t count, loff_t *ppos)
{
	size_t bytes_left = raw_mSBC_index - *ppos;
	if (count > bytes_left)
		count = bytes_left;
	if (copy_to_user(buffer, &raw_mSBC_buffer[*ppos], count))
		return -EFAULT;

	*ppos += count;
	return count;
}

static const struct file_operations mSBC_fops = {
	.owner = THIS_MODULE,
	.open = mSBC_dev_open,
	.llseek = no_llseek,
	.read = mSBC_dev_read,
};

#endif

/*
 * Static substream is needed so Bluetooth can pass encoded audio
 * to a running stream.
 * This also serves to enable or disable the decoding of audio in the callback.
 */
static struct snd_pcm_substream *s_substream_for_btle;
static DEFINE_SPINLOCK(s_substream_lock);

struct simple_atomic_fifo {
	/* Read and write cursors are modified by different threads. */
	uint read_cursor;
	uint write_cursor;
	/* Size must be a power of two. */
	uint size;
	/* internal mask is 2*size - 1
	 * This allows us to tell the difference between full and empty. */
	uint internal_mask;
	uint external_mask;
};

struct snd_atvr {
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_pcm_hardware pcm_hw;

	uint32_t sample_rate;

	uint previous_jiffies; /* Used to detect underflows. */
	uint timeout_jiffies;
	struct timer_list decoding_timer;
	uint timer_state;
	bool timer_enabled;
	uint timer_callback_count;

	int16_t peak_level;
	struct simple_atomic_fifo fifo_controller;
	struct fifo_packet *fifo_packet_buffer;

	/* IMA/DVI ADPCM Decoder */
	int pcm_value;
	int step_index;
	bool first_packet;

	/* mSBC decoder */
	uint8_t mSBC_frame_data[BYTES_PER_MSBC_FRAME];
	int16_t audio_output[MAX_SAMPLES_PER_PACKET];
	uint8_t packet_in_frame;
	uint8_t seq_index;

	/*
	 * Write_index is the circular buffer position.
	 * It is advanced by the BTLE thread after decoding.
	 * It is read by ALSA in snd_atvr_pcm_pointer().
	 * It is not declared volatile because that is not
	 * allowed in the Linux kernel.
	 */
	uint32_t write_index;
	uint32_t frames_per_buffer;
	/* count frames generated so far in this period */
	uint32_t frames_in_period;
	int16_t *pcm_buffer;

};

/***************************************************************************/
/************* Atomic FIFO *************************************************/
/***************************************************************************/
/*
 * This FIFO is atomic if used by no more than 2 threads.
 * One thread modifies the read cursor and the other
 * thread modifies the write_cursor.
 * Size and mask are not modified while being used.
 *
 * The read and write cursors range internally from 0 to (2*size)-1.
 * This allows us to tell the difference between full and empty.
 * When we get the cursors for external use we mask with size-1.
 *
 * Memory barriers required on SMP platforms.
 */
static int atomic_fifo_init(struct simple_atomic_fifo *fifo_ptr, uint size)
{
	/* Make sure size is a power of 2. */
	if ((size & (size-1)) != 0) {
		pr_err("%s:%d - ERROR FIFO size = %d, not power of 2!\n",
			__func__, __LINE__, size);
		return -EINVAL;
	}
	fifo_ptr->read_cursor = 0;
	fifo_ptr->write_cursor = 0;
	fifo_ptr->size = size;
	fifo_ptr->internal_mask = (size * 2) - 1;
	fifo_ptr->external_mask = size - 1;
	smp_wmb();
	return 0;
}


static uint atomic_fifo_available_to_read(struct simple_atomic_fifo *fifo_ptr)
{
	smp_rmb();
	return (fifo_ptr->write_cursor - fifo_ptr->read_cursor)
			& fifo_ptr->internal_mask;
}

static uint atomic_fifo_available_to_write(struct simple_atomic_fifo *fifo_ptr)
{
	smp_rmb();
	return fifo_ptr->size - atomic_fifo_available_to_read(fifo_ptr);
}

static void atomic_fifo_advance_read(
		struct simple_atomic_fifo *fifo_ptr,
		uint frames)
{
	smp_rmb();
	BUG_ON(frames > atomic_fifo_available_to_read(fifo_ptr));
	fifo_ptr->read_cursor = (fifo_ptr->read_cursor + frames)
			& fifo_ptr->internal_mask;
	smp_wmb();
}

static void atomic_fifo_advance_write(
		struct simple_atomic_fifo *fifo_ptr,
		uint frames)
{
	smp_rmb();
	BUG_ON(frames > atomic_fifo_available_to_write(fifo_ptr));
	fifo_ptr->write_cursor = (fifo_ptr->write_cursor + frames)
		& fifo_ptr->internal_mask;
	smp_wmb();
}

static uint atomic_fifo_get_read_index(struct simple_atomic_fifo *fifo_ptr)
{
	smp_rmb();
	return fifo_ptr->read_cursor & fifo_ptr->external_mask;
}

static uint atomic_fifo_get_write_index(struct simple_atomic_fifo *fifo_ptr)
{
	smp_rmb();
	return fifo_ptr->write_cursor & fifo_ptr->external_mask;
}

/****************************************************************************/
static void snd_atvr_handle_frame_advance(
		struct snd_pcm_substream *substream, uint num_frames)
{
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
	atvr_snd->frames_in_period += num_frames;
	/* Tell ALSA if we have advanced by one or more periods. */
	if (atvr_snd->frames_in_period >= substream->runtime->period_size) {
		snd_pcm_period_elapsed(substream);
		atvr_snd->frames_in_period %= substream->runtime->period_size;
	}
}

static uint32_t snd_atvr_bump_write_index(
			struct snd_pcm_substream *substream,
			uint32_t num_samples)
{
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
	uint32_t pos = atvr_snd->write_index;

	/* Advance write position. */
	pos += num_samples;
	/* Wrap around at end of the circular buffer. */
	pos %= atvr_snd->frames_per_buffer;
	atvr_snd->write_index = pos;

	snd_atvr_handle_frame_advance(substream, num_samples);

	return pos;
}

/*
 * Decode an IMA/DVI ADPCM packet and write the PCM data into a circular buffer.
 * ADPCM is 4:1 16kHz@256kbps -> 16kHz@64kbps.
 * ADPCM is 4:1 8kHz@128kbps -> 8kHz@32kbps.
 */
static const int ima_index_table[16] = {
	-1, -1, -1, -1, /* +0 - +3, decrease the step size */
	2, 4, 6, 8,     /* +4 - +7, increase the step size */
	-1, -1, -1, -1, /* -0 - -3, decrease the step size */
	2, 4, 6, 8      /* -4 - -7, increase the step size */
};
static const int16_t ima_step_table[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
	19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
	50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
	337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
	5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static void decode_adpcm_nibble(uint8_t nibble, struct snd_atvr *atvr_snd,
				struct snd_pcm_substream *substream)
{
	int step_index = atvr_snd->step_index;
	int value = atvr_snd->pcm_value;
	int step = ima_step_table[step_index];
	int diff;

#if 0
	static int ii = 0;
	if(ii == 50){
		printk("%x ", nibble);
		printk("\n");
		ii = 0;
	}else {
		ii++;
		printk("%x %x %x %x ", value,step_index, step, nibble);
	}
#endif

	diff = step >> 3;
	if (nibble & 1)
		diff += (step >> 2);
	if (nibble & 2)
		diff += (step >> 1);
	if (nibble & 4)
		diff += step;

	if (nibble & 8) {
		value -= diff;
		if (value < -32768)
			value = -32768;
	} else {
		value += diff;
		if (value > 32767)
			value = 32767;
	}
	atvr_snd->pcm_value = value;

//	printk("v:%x ", value);

	/* copy to stream */
	atvr_snd->pcm_buffer[atvr_snd->write_index] = value;
#if (DEBUG_WITH_MISC_DEVICE == 1)
	if (large_pcm_index < ARRAY_SIZE(large_pcm_buffer))
		large_pcm_buffer[large_pcm_index++] = value;
#endif
	snd_atvr_bump_write_index(substream, 1);
	if (value > atvr_snd->peak_level)
		atvr_snd->peak_level = value;

	/* update step_index */
	step_index += ima_index_table[nibble];
	/* clamp step_index */
	if (step_index < 0)
		step_index = 0;
	else if (step_index >= ARRAY_SIZE(ima_step_table))
		step_index = ARRAY_SIZE(ima_step_table) - 1;
	atvr_snd->step_index = step_index;
}

static int snd_atvr_decode_adpcm_packet(
			struct snd_pcm_substream *substream,
			const uint8_t *adpcm_input,
			size_t num_bytes
			)
{
	uint i;
	unsigned  char *vl; 
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);

	/* Decode IMA ADPCM data to PCM. */
	if (atvr_snd->first_packet) {
		/* the first two bytes of the first packet
		 * is the unencoded first 16-bit sample, high
		 * byte first.
		 */
		static int value;
		signed short *pd = (signed short *)adpcm_input;
		//int value = ((int)adpcm_input[0] << 8) | adpcm_input[1];
		value = pd[0];
//		 pr_info("miles:%s: first packet, initial value is 0x%x D:%d (0x%x, 0x%x)\n",
//			__func__, value, value, adpcm_input[0], adpcm_input[1]);
if(adpcm_input[0] == 0xfe && adpcm_input[1] == 0xff)
		pr_info("miles:%s: first packet, initial value is %d (0x%x, 0x%x) num_bytes:%lu\n",
			__func__, value, adpcm_input[0], adpcm_input[1], num_bytes);
		atvr_snd->pcm_value = value;
		atvr_snd->pcm_buffer[atvr_snd->write_index] = value;
#if (DEBUG_WITH_MISC_DEVICE == 1)
		if (raw_adpcm_index < ARRAY_SIZE(raw_adpcm_buffer))
			raw_adpcm_buffer[raw_adpcm_index++] = adpcm_input[0];
		if (raw_adpcm_index < ARRAY_SIZE(raw_adpcm_buffer))
			raw_adpcm_buffer[raw_adpcm_index++] = adpcm_input[1];
		if (large_pcm_index < ARRAY_SIZE(large_pcm_buffer))
			large_pcm_buffer[large_pcm_index++] = value;
#endif
		snd_atvr_bump_write_index(substream, 1);
		atvr_snd->peak_level = value;
#ifndef EACH_WITH_HEAD_MODE // Every notification with 2B ADPCM
		atvr_snd->first_packet = false;
#else
		atvr_snd->step_index = 0;
#endif
		i = 2;
	} else {
		i = 0;
	}

//rev adpcm, zewen
#if 0
	static int it = 0;
	if(it <500){
	int ii;
	for(ii = 0; ii< num_bytes;ii++)
		printk("%x ",adpcm_input[ii]);
	printk("\n");
	it++;
	}
#endif

//printk("%x %x ", adpcm_input[0], adpcm_input[1]);

	for (; i < num_bytes; i++) {
		uint8_t raw = adpcm_input[i];
		uint8_t nibble;

#if (DEBUG_WITH_MISC_DEVICE == 1)
		if (raw_adpcm_index < ARRAY_SIZE(raw_adpcm_buffer))
			raw_adpcm_buffer[raw_adpcm_index++] = raw;
#endif
		//unsigned  char *vl; 
		/* process first nibble */
		nibble = (raw >> 4) & 0x0f;
		decode_adpcm_nibble(nibble, atvr_snd, substream);
		vl = (unsigned char *)&atvr_snd->pcm_value;
//printk("%x %x ", *vl, *(vl+1)); //rev pcm, zewen
		/* process second nibble */
		nibble = raw & 0x0f;
		decode_adpcm_nibble(nibble, atvr_snd, substream);
		vl = (unsigned char *)&atvr_snd->pcm_value;
//printk("%x %x ", *vl, *(vl+1));//rev pcm, zewen
//printk("%x ", atvr_snd->pcm_value);
	}
//printk("\n");
	return num_bytes * 2;
}

/*
 * Decode an mSBC packet and write the PCM data into a circular buffer.
 * mSBC is supposed to be 16KHz but this is a 8KHz variant version.
 */
#define BLOCKS_PER_PACKET 15
#define NUM_BITS 26

static int snd_atvr_decode_8KHz_mSBC_packet(
			struct snd_pcm_substream *substream,
			const uint8_t *sbc_input,
			size_t num_bytes
			)
{
	uint num_samples = 0;
	uint remaining;
	uint i;
	uint32_t pos;
	uint read_index;
	uint write_index;
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);

	/* Decode mSBC data to PCM. */
#if (DEBUG_WITH_MISC_DEVICE == 1)
	for (i = 0; i < num_bytes; i++) {
		if (raw_mSBC_index < ARRAY_SIZE(raw_mSBC_buffer))
			raw_mSBC_buffer[raw_mSBC_index++] = sbc_input[i];
		else
			break;
	}
#endif
	if (atvr_snd->packet_in_frame == 0) {
		if (sbc_input[0] != mSBC_sequence_table[atvr_snd->seq_index]) {
			snd_atvr_log("sequence_num err, 0x%02x != 0x%02x\n",
				     sbc_input[1],
				     mSBC_sequence_table[atvr_snd->seq_index]);
			return 0;
		}
		atvr_snd->seq_index++;
		if (atvr_snd->seq_index == NUM_SEQUENCES)
			atvr_snd->seq_index = 0;

		/* subtract the sequence number */
		num_bytes--;
	}
	if (num_bytes != mSBC_bytes_in_packet[atvr_snd->packet_in_frame]) {
		pr_err("%s: received %zd audio bytes but expected %d bytes\n",
		       __func__, num_bytes,
		       mSBC_bytes_in_packet[atvr_snd->packet_in_frame]);
		return 0;
	}
	write_index = mSBC_start_offset_in_buffer[atvr_snd->packet_in_frame];
	read_index = mSBC_start_offset_in_packet[atvr_snd->packet_in_frame];
	memcpy(&atvr_snd->mSBC_frame_data[write_index],
	       &sbc_input[read_index],
	       mSBC_bytes_in_packet[atvr_snd->packet_in_frame]);
	atvr_snd->packet_in_frame++;
	if (atvr_snd->packet_in_frame < BLE_PACKETS_PER_MSBC_FRAME) {
		/* we don't have a complete mSBC frame yet, just return */
		return 0;
	}
	/* reset for next mSBC frame */
	atvr_snd->packet_in_frame = 0;

	/* we have a complete mSBC frame, send it to the decoder */
	num_samples = sbc_decode(BLOCKS_PER_PACKET, NUM_BITS,
				 atvr_snd->mSBC_frame_data,
				 BYTES_PER_MSBC_FRAME,
				 &atvr_snd->audio_output[0]);

	/* Write PCM data to the buffer. */
	pos = atvr_snd->write_index;
	read_index = 0;
	if ((pos + num_samples) > atvr_snd->frames_per_buffer) {
		for (i = pos; i < atvr_snd->frames_per_buffer; i++) {
			int16_t sample = atvr_snd->audio_output[read_index++];
			if (sample > atvr_snd->peak_level)
				atvr_snd->peak_level = sample;
#if (DEBUG_WITH_MISC_DEVICE == 1)
			if (large_pcm_index < ARRAY_SIZE(large_pcm_buffer))
				large_pcm_buffer[large_pcm_index++] = sample;
#endif
			atvr_snd->pcm_buffer[i] = sample;
		}

		remaining = (pos + num_samples) - atvr_snd->frames_per_buffer;
		for (i = 0; i < remaining; i++) {
			int16_t sample = atvr_snd->audio_output[read_index++];
			if (sample > atvr_snd->peak_level)
				atvr_snd->peak_level = sample;
#if (DEBUG_WITH_MISC_DEVICE == 1)
			if (large_pcm_index < ARRAY_SIZE(large_pcm_buffer))
				large_pcm_buffer[large_pcm_index++] = sample;
#endif

			atvr_snd->pcm_buffer[i] = sample;
		}

	} else {
		for (i = 0; i < num_samples; i++) {
			int16_t sample = atvr_snd->audio_output[read_index++];
			if (sample > atvr_snd->peak_level)
				atvr_snd->peak_level = sample;
#if (DEBUG_WITH_MISC_DEVICE == 1)
			if (large_pcm_index < ARRAY_SIZE(large_pcm_buffer))
				large_pcm_buffer[large_pcm_index++] = sample;
#endif
			atvr_snd->pcm_buffer[i + pos] = sample;
		}
	}

	snd_atvr_bump_write_index(substream, num_samples);

	return num_samples;
}

/**
 * This is called by the event filter when it gets an audio packet
 * from the AndroidTV remote.  It writes the packet into a FIFO
 * which is then read and decoded by the timer task.
 * @param input pointer to data to be decoded
 * @param num_bytes how many bytes in raw_input
 * @return number of samples decoded or negative error.
 */
static void audio_dec(const uint8_t *raw_input, int type, size_t num_bytes)
{
	bool dropped_packet = false;
	struct snd_pcm_substream *substream;

	spin_lock(&s_substream_lock);
	substream = s_substream_for_btle;
	if (substream != NULL) {
		struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
		/* Write data to a FIFO for decoding by the timer task. */
		uint writable = atomic_fifo_available_to_write(
			&atvr_snd->fifo_controller);
		if (writable > 0) {
			uint fifo_index = atomic_fifo_get_write_index(
				&atvr_snd->fifo_controller);
			struct fifo_packet *packet =
				&atvr_snd->fifo_packet_buffer[fifo_index];
			packet->type = type;
			packet->num_bytes = (uint8_t)num_bytes;
			memcpy(packet->raw_data, raw_input, num_bytes);
			atomic_fifo_advance_write(
				&atvr_snd->fifo_controller, 1);
		} else {
			dropped_packet = true;
			s_substream_for_btle = NULL; /* Stop decoding. */
		}
	}
	packet_counter++;
	spin_unlock(&s_substream_lock);

	if (dropped_packet)
		snd_atvr_log("WARNING, raw audio packet dropped, FIFO full\n");
}

/*
 * Note that smp_rmb() is called by snd_atvr_timer_callback()
 * before calling this function.
 *
 * Reads:
 *    jiffies
 *    atvr_snd->previous_jiffies
 * Writes:
 *    atvr_snd->previous_jiffies
 * Returns:
 *    num_frames needed to catch up to the current time
 */
static uint snd_atvr_calc_frame_advance(struct snd_atvr *atvr_snd)
{
	/* Determine how much time passed. */
	uint now_jiffies = jiffies;
	uint elapsed_jiffies = now_jiffies - atvr_snd->previous_jiffies;
	/* Convert jiffies to frames. */
	uint frames_by_time = jiffies_to_msecs(elapsed_jiffies)
		* atvr_snd->sample_rate / 1000;
	atvr_snd->previous_jiffies = now_jiffies;

	/* Don't write more than one buffer full. */
	if (frames_by_time > (atvr_snd->frames_per_buffer - 4))
		frames_by_time  = atvr_snd->frames_per_buffer - 4;

	return frames_by_time;
}

/* Write zeros into the PCM buffer. */
static uint32_t snd_atvr_write_silence(struct snd_atvr *atvr_snd,
			uint32_t pos,
			int frames_to_advance)
{
	/* Does it wrap? */
	if ((pos + frames_to_advance) > atvr_snd->frames_per_buffer) {
		/* Write to end of buffer. */
		int16_t *destination = &atvr_snd->pcm_buffer[pos];
		size_t num_frames = atvr_snd->frames_per_buffer - pos;
		size_t num_bytes = num_frames * sizeof(int16_t);
		memset(destination, 0, num_bytes);
		/* Write from start of buffer to new pos. */
		destination = &atvr_snd->pcm_buffer[0];
		num_frames = frames_to_advance - num_frames;
		num_bytes = num_frames * sizeof(int16_t);
		memset(destination, 0, num_bytes);
	} else {
		/* Write within the buffer. */
		int16_t *destination = &atvr_snd->pcm_buffer[pos];
		size_t num_bytes = frames_to_advance * sizeof(int16_t);
		memset(destination, 0, num_bytes);
	}
	/* Advance and wrap write_index */
	pos += frames_to_advance;
	pos %= atvr_snd->frames_per_buffer;
	return pos;
}

/*
 * Called by timer task to decode raw audio data from the FIFO into the PCM
 * buffer.  Returns the number of packets decoded.
 */
static uint snd_atvr_decode_from_fifo(struct snd_pcm_substream *substream)
{
	uint i;
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
	uint readable = atomic_fifo_available_to_read(
		&atvr_snd->fifo_controller);
	for (i = 0; i < readable; i++) {
		uint fifo_index = atomic_fifo_get_read_index(
			&atvr_snd->fifo_controller);
		struct fifo_packet *packet =
			&atvr_snd->fifo_packet_buffer[fifo_index];
		if (packet->type == PACKET_TYPE_ADPCM) {
			snd_atvr_decode_adpcm_packet(substream,
						     packet->raw_data,
						     packet->num_bytes);
		} else if (packet->type == PACKET_TYPE_MSBC) {
			snd_atvr_decode_8KHz_mSBC_packet(substream,
							 packet->raw_data,
							 packet->num_bytes);
		} else {
			pr_err("Unknown packet type %d\n", packet->type);
		}

		atomic_fifo_advance_read(&atvr_snd->fifo_controller, 1);
	}
	return readable;
}

static int snd_atvr_schedule_timer(struct snd_pcm_substream *substream)
{
	int ret;
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
	uint msec_to_sleep = (substream->runtime->period_size * 1000)
			/ atvr_snd->sample_rate;
	uint jiffies_to_sleep = msecs_to_jiffies(msec_to_sleep);
	if (jiffies_to_sleep < 2)
		jiffies_to_sleep = 2;
	ret = mod_timer(&atvr_snd->decoding_timer, jiffies + jiffies_to_sleep);
	if (ret < 0)
		pr_err("%s:%d - ERROR in mod_timer, ret = %d\n",
			   __func__, __LINE__, ret);
	return ret;
}

static void snd_atvr_timer_callback(unsigned long data)
{
	uint readable;
	uint packets_read;
	bool need_silence = false;
	struct snd_pcm_substream *substream = (struct snd_pcm_substream *)data;
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);

	/* timer_enabled will be false when stopping a stream. */
	smp_rmb();
	if (!atvr_snd->timer_enabled)
		return;
	atvr_snd->timer_callback_count++;

	switch (atvr_snd->timer_state) {
	case TIMER_STATE_BEFORE_DECODE:
		readable = atomic_fifo_available_to_read(
				&atvr_snd->fifo_controller);
		if (readable > 0) {
			atvr_snd->timer_state = TIMER_STATE_DURING_DECODE;
			/* Fall through into next state. */
		} else {
			need_silence = true;
			break;
		}

	case TIMER_STATE_DURING_DECODE:
		packets_read = snd_atvr_decode_from_fifo(substream);
		if (packets_read > 0) {
			/* Defer timeout */
			atvr_snd->previous_jiffies = jiffies;
			break;
		}
		if (s_substream_for_btle == NULL) {
			atvr_snd->timer_state = TIMER_STATE_AFTER_DECODE;
			/* Decoder died. Overflowed?
			 * Fall through into next state. */
		} else if ((jiffies - atvr_snd->previous_jiffies) >
			   atvr_snd->timeout_jiffies) {
			snd_atvr_log("audio UNDERFLOW detected\n");
			/*  Not fatal.  Reset timeout. */
			atvr_snd->previous_jiffies = jiffies;
			break;
		} else
			break;

	case TIMER_STATE_AFTER_DECODE:
		need_silence = true;
		break;
	}

	/* Write silence before and after decoding. */
	if (need_silence) {
		uint frames_to_silence = snd_atvr_calc_frame_advance(atvr_snd);
		atvr_snd->write_index = snd_atvr_write_silence(
				atvr_snd,
				atvr_snd->write_index,
				frames_to_silence);
		/* This can cause snd_atvr_pcm_trigger() to be called, which
		 * may try to stop the timer. */
		snd_atvr_handle_frame_advance(substream, frames_to_silence);
	}

	smp_rmb();
	if (atvr_snd->timer_enabled)
		snd_atvr_schedule_timer(substream);
}

static void snd_atvr_timer_start(struct snd_pcm_substream *substream)
{
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
	atvr_snd->timer_enabled = true;
	atvr_snd->previous_jiffies = jiffies;
	atvr_snd->timeout_jiffies =
		msecs_to_jiffies(SND_ATVR_RUNNING_TIMEOUT_MSEC);
	atvr_snd->timer_callback_count = 0;
	smp_wmb();
	setup_timer(&atvr_snd->decoding_timer,
		snd_atvr_timer_callback,
		(unsigned long)substream);


	snd_atvr_schedule_timer(substream);
}


static void snd_atvr_timer_stop(struct snd_pcm_substream *substream)
{
	int ret;
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);

	/* Tell timer function not to reschedule itself if it runs. */
	atvr_snd->timer_enabled = false;
	smp_wmb();
	if (!in_interrupt()) {
		/* del_timer_sync will hang if called in the timer callback. */
		ret = del_timer_sync(&atvr_snd->decoding_timer);
		if (ret < 0)
			pr_err("%s:%d - ERROR del_timer_sync failed, %d\n",
				__func__, __LINE__, ret);
	}
	/*
	 * Else if we are in an interrupt then we are being called from the
	 * middle of the snd_atvr_timer_callback(). The timer will not get
	 * rescheduled because atvr_snd->timer_enabled will be false
	 * at the end of snd_atvr_timer_callback().
	 * We do not need to "delete" the timer.
	 * The del_timer functions just cancel pending timers.
	 * There are no resources that need to be cleaned up.
	 */
}

/* ===================================================================== */
/*
 * PCM interface
 */

static int snd_atvr_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
	printk("miles---> [FUNC]:%s [LINE]:%d\n",  __FUNCTION__, __LINE__);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		snd_atvr_log("%s starting audio\n", __func__);

#if (DEBUG_WITH_MISC_DEVICE == 1)
		large_pcm_index = 0;
		raw_adpcm_index = 0;
		raw_mSBC_index = 0;
#endif
		packet_counter = 0;
		atvr_snd->peak_level = -32768;
		atvr_snd->previous_jiffies = jiffies;
		atvr_snd->timer_state = TIMER_STATE_BEFORE_DECODE;

		/* ADPCM decoder state */
		atvr_snd->step_index = 0;
		atvr_snd->pcm_value = 0;
		atvr_snd->first_packet = true;

		/* mSBC decoder */
		atvr_snd->packet_in_frame = 0;
		atvr_snd->seq_index = 0;

		snd_atvr_timer_start(substream);
#ifdef TEST_ADPCM_DATA_FILL
        fake_notifiy_index =0;
        filldata_test_timer_start();
#endif
		rev_packet_num = 0;//zewen add
		memset(audio_rev_buf, 0, sizeof(audio_rev_buf));

		 /* Enables callback from BTLE driver. */
		s_substream_for_btle = substream;
		smp_wmb(); /* so other thread will see s_substream_for_btle */
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		snd_atvr_log("%s stopping audio, peak = %d, # packets = %d\n",
			__func__, atvr_snd->peak_level, packet_counter);

		s_substream_for_btle = NULL;
		smp_wmb(); /* so other thread will see s_substream_for_btle */
		snd_atvr_timer_stop(substream);
#ifdef TEST_ADPCM_DATA_FILL
        stop_test_timer();
#endif
		return 0;
	}
	return -EINVAL;
}

static int snd_atvr_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_atvr_log("%s, rate = %d, period_size = %d, buffer_size = %d\n",
		__func__, (int) runtime->rate,
		(int) runtime->period_size,
		(int) runtime->buffer_size);

	if (runtime->buffer_size > MAX_FRAMES_PER_BUFFER)
		return -EINVAL;

	atvr_snd->sample_rate = runtime->rate;
	atvr_snd->frames_per_buffer = runtime->buffer_size;

	return 0; /* TODO - review */
}

static struct snd_pcm_hardware atvr_pcm_hardware;//= {
#if 0
	.info =			(SNDRV_PCM_INFO_MMAP |
				 SNDRV_PCM_INFO_INTERLEAVED |
				 SNDRV_PCM_INFO_RESUME |
				 SNDRV_PCM_INFO_MMAP_VALID),
	.formats =		USE_FORMATS,
	.rates =		USE_RATES_MASK,
	.rate_min =		USE_RATE_MIN,
	.rate_max =		USE_RATE_MAX,
	.channels_min =		USE_CHANNELS_MIN,
	.channels_max =		USE_CHANNELS_MAX,
	.buffer_bytes_max =	MAX_PCM_BUFFER_SIZE,
	.period_bytes_min =	MIN_PERIOD_SIZE,
	.period_bytes_max =	MAX_PERIOD_SIZE,
	.periods_min =		USE_PERIODS_MIN,
	.periods_max =		USE_PERIODS_MAX,
	.fifo_size =		0,
};
#endif 

static int snd_atvr_pcm_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *hw_params)
{
	int ret = 0;
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);

	atvr_snd->write_index = 0;
	smp_wmb();

	return ret;
}

static int snd_atvr_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return 0;
}

static int snd_atvr_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	int ret = atomic_fifo_init(&atvr_snd->fifo_controller,
				   MAX_PACKETS_PER_BUFFER);
	if (ret)
		return ret;

	runtime->hw = atvr_snd->pcm_hw;
	if (substream->pcm->device & 1) {
		runtime->hw.info &= ~SNDRV_PCM_INFO_INTERLEAVED;
		runtime->hw.info |= SNDRV_PCM_INFO_NONINTERLEAVED;
	}
	if (substream->pcm->device & 2)
		runtime->hw.info &= ~(SNDRV_PCM_INFO_MMAP
			| SNDRV_PCM_INFO_MMAP_VALID);

	//snd_atvr_log("%s, built %s %s\n", __func__, __DATE__, __TIME__);

	/*
	 * Allocate the maximum buffer now and then just use part of it when
	 * the substream starts. We don't need DMA because it will just
	 * get written to by the BTLE code.
	 */
	/* We only use this buffer in the kernel and we do not do
	 * DMA so vmalloc should be OK. */
	atvr_snd->pcm_buffer = vmalloc(MAX_PCM_BUFFER_SIZE);
	if (atvr_snd->pcm_buffer == NULL) {
		pr_err("%s:%d - ERROR PCM buffer allocation failed\n",
			__func__, __LINE__);
		return -ENOMEM;
	}

	/* We only use this buffer in the kernel and we do not do
	 * DMA so vmalloc should be OK.
	 */
	atvr_snd->fifo_packet_buffer = vmalloc(MAX_BUFFER_SIZE);
	if (atvr_snd->fifo_packet_buffer == NULL) {
		pr_err("%s:%d - ERROR buffer allocation failed\n",
			__func__, __LINE__);
		vfree(atvr_snd->pcm_buffer);
		atvr_snd->pcm_buffer = NULL;
		return -ENOMEM;
	}

	return 0;
}

static int snd_atvr_pcm_close(struct snd_pcm_substream *substream)
{
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);

	/* Make sure the timer is not running */
	if (atvr_snd->timer_enabled)
		snd_atvr_timer_stop(substream);

	if (atvr_snd->timer_callback_count > 0)
		snd_atvr_log("processed %d packets in %d timer callbacks\n",
			packet_counter, atvr_snd->timer_callback_count);

	if (atvr_snd->pcm_buffer) {
		vfree(atvr_snd->pcm_buffer);
		atvr_snd->pcm_buffer = NULL;
	}

	/*
	 * Use spinlock so we don't free the FIFO when the
	 * driver is writing to it.
	 * The s_substream_for_btle should already be NULL by now.
	 */
	spin_lock(&s_substream_lock);
	if (atvr_snd->fifo_packet_buffer) {
		vfree(atvr_snd->fifo_packet_buffer);
		atvr_snd->fifo_packet_buffer = NULL;
	}
	spin_unlock(&s_substream_lock);
	return 0;
}

static snd_pcm_uframes_t snd_atvr_pcm_pointer(
		struct snd_pcm_substream *substream)
{
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
	/* write_index is written by another driver thread */
	smp_rmb();
	return atvr_snd->write_index;
}

static int snd_atvr_pcm_copy(struct snd_pcm_substream *substream,
			  int channel, snd_pcm_uframes_t pos,
			  void __user *dst, snd_pcm_uframes_t count)
{
	struct snd_atvr *atvr_snd = snd_pcm_substream_chip(substream);
	short *output = (short *)dst;

	/* TODO Needs to be modified if we support more than 1 channel. */
	/*
	 * Copy from PCM buffer to user memory.
	 * Are we reading past the end of the buffer?
	 */
	if ((pos + count) > atvr_snd->frames_per_buffer) {
		const int16_t *source = &atvr_snd->pcm_buffer[pos];
		int16_t *destination = output;
		size_t num_frames = atvr_snd->frames_per_buffer - pos;
		size_t num_bytes = num_frames * sizeof(int16_t);
		memcpy(destination, source, num_bytes);

		source = &atvr_snd->pcm_buffer[0];
		destination += num_frames;
		num_frames = count - num_frames;
		num_bytes = num_frames * sizeof(int16_t);
		memcpy(destination, source, num_bytes);
	} else {
		const int16_t *source = &atvr_snd->pcm_buffer[pos];
		int16_t *destination = output;
		size_t num_bytes = count * sizeof(int16_t);
		memcpy(destination, source, num_bytes);
	}

	return 0;
}

static int snd_atvr_pcm_silence(struct snd_pcm_substream *substream,
				int channel, snd_pcm_uframes_t pos,
				snd_pcm_uframes_t count)
{
	return 0; /* Do nothing. Only used by output? */
}

static struct snd_pcm_ops snd_atvr_pcm_ops_no_buf = {
	.open =		snd_atvr_pcm_open,
	.close =	snd_atvr_pcm_close,
	.ioctl =	snd_pcm_lib_ioctl,
	.hw_params =	snd_atvr_pcm_hw_params,
	.hw_free =	snd_atvr_pcm_hw_free,
	.prepare =	snd_atvr_pcm_prepare,
	.trigger =	snd_atvr_pcm_trigger,
	.pointer =	snd_atvr_pcm_pointer,
	.copy =		snd_atvr_pcm_copy,
	.silence =	snd_atvr_pcm_silence,
};

static int snd_card_atvr_pcm(struct snd_atvr *atvr_snd,
			     int device,
			     int substreams)
{
	struct snd_pcm *pcm;
	struct snd_pcm_ops *ops;
	int err;

	err = snd_pcm_new(atvr_snd->card, "ATVR PCM", device,
			  0, /* no playback substreams */
			  1, /* 1 capture substream */
			  &pcm);
	if (err < 0)
		return err;
	atvr_snd->pcm = pcm;
	ops = &snd_atvr_pcm_ops_no_buf;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, ops);
	pcm->private_data = atvr_snd;
	pcm->info_flags = 0;
	strcpy(pcm->name, "ATVR PCM");

	return 0;
}

static int atvr_snd_initialize(struct hid_device *hdev)
{
	struct snd_card *card;
	struct snd_atvr *atvr_snd;
	int err;
	int i;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}
	printk("zewen---> [FUNC]%s [LINE]:%d\n", __FUNCTION__, __LINE__);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,15,0)
	err = snd_card_create(index[dev], id[dev], THIS_MODULE,
			      sizeof(struct snd_atvr), &card);
#else
	if(&hdev->dev ==NULL)
		printk("zewen---> [FUNC]%s [LINE]:%d dev is NULL!!\n", __FUNCTION__, __LINE__);
	else
		printk("zewen---> [FUNC]%s [LINE]:%d dev is not NULL!!!\n", __FUNCTION__, __LINE__);
	err = snd_card_new(&hdev->dev,index[dev], id[dev], THIS_MODULE,
			      sizeof(struct snd_atvr), &card);
#endif
	if (err < 0) {
		pr_err("%s: snd_card_create() returned err %d\n",
		       __func__, err);
		return err;
	}
	printk("zewen---> [FUNC]%s [LINE]:%d\n", __FUNCTION__, __LINE__);
        pcard = card;

	hid_set_drvdata(hdev, card);
	atvr_snd = card->private_data;
	atvr_snd->card = card;
	for (i = 0; i < MAX_PCM_DEVICES && i < pcm_devs[dev]; i++) {
		if (pcm_substreams[dev] < 1)
			pcm_substreams[dev] = 1;
		if (pcm_substreams[dev] > MAX_PCM_SUBSTREAMS)
			pcm_substreams[dev] = MAX_PCM_SUBSTREAMS;
		err = snd_card_atvr_pcm(atvr_snd, i, pcm_substreams[dev]);
		if (err < 0) {
			pr_err("%s: snd_card_atvr_pcm() returned err %d\n",
			       __func__, err);
			goto __nodev;
		}
	}


	atvr_snd->pcm_hw = atvr_pcm_hardware;

	strcpy(card->driver, "Telink Remote ");
	strcpy(card->shortname, "TelinkAudio");
	sprintf(card->longname, "Telink Remote %i audio", dev + 1);


	snd_card_set_dev(card, &hdev->dev);

	err = snd_card_register(card);
	if (!err)
		return 0;

__nodev:
	snd_card_free(card);
	return err;
}

int p_n = 0;
int i_t = 0;
static int atvr_raw_event(struct hid_device *hdev, struct hid_report *report,
		u8 *data, int size)
{
	//	printk("miles---> [FUNC]:%s [LINE]:%d\n",  __FUNCTION__, __LINE__);
#if (DEBUG_HID_RAW_INPUT == 1)
	//	pr_info("%s: report->id = 0x%x, size = %d\n",
	//		__func__, report->id, size);
	//if(data[1]==0xfe && data[2] == 0xff){
	static int it=0;
//	int ii;
	//printk("it:%d size:%d\n",it, size);
//	if(it<100){
		//if (size < 32) {
		if (size < 132) {
			int i;
			for (i = 1; i < size; i++) {
				//pr_info("data[%d] = 0x%02x\n", i, data[i]);
			//	if(i == 1)
			//		printk("%d ",  data[i]);
			//	else
				//	printk("%x ",  data[i]);//rev adpcm, from remote, zewen
					//printk("\b");
			}
		}
		//printk("\n");//rev adpcm, from remote, zewen
	//	it++;
//	}
#endif
	if (report->id == 0x07 || report->id == 0x06 || report->id == 0x05 || report->id == ADPCM_AUDIO_REPORT_ID) {
		/* send the data, minus the report-id in data[0], to the
		 * alsa audio decoder driver for ADPCM
		 */
#if (DEBUG_AUDIO_RECEPTION == 1)
		if (packet_counter == 0) {
			snd_atvr_log("first ADPCM packet received\n");
		}
#endif
	//	int ii;
	//	printk("miles---> [FUNC]:%s [LINE]:%d\n",  __FUNCTION__, __LINE__);
	//	for(ii = 1; ii<21; ii++)
	//		printk("%x ", data[ii]);
	//	printk("\n");
#if 0
		if(rev_packet_num < 6)
		{
			memcpy(audio_rev_buf + rev_packet_num * 20, &data[1], 20);
			rev_packet_num++;
	//		p_n++;
		}
#if 1
		else if(rev_packet_num == 6 || size == 7)
		{
			memcpy(audio_rev_buf + rev_packet_num * 20, &data[1], 6);
			rev_packet_num = 0;
	//		p_n++;
			audio_dec(audio_rev_buf, PACKET_TYPE_ADPCM, 126);
			memset(audio_rev_buf, 0, sizeof(audio_rev_buf));
		}
#endif
#if 0
		if(i_t == 0)
		{
			if(p_n == 42)
			{
				rev_packet_num = 0;
				i_t++;
			}
		}else{
			if(p_n == 40)
			{
				rev_packet_num = 0;
			}
		}
#endif 
#endif
		audio_dec(&data[1], PACKET_TYPE_ADPCM, size - 1);
		//audio_dec(&data[2], PACKET_TYPE_ADPCM, size - 2);
		/* we've handled the event */
		return 1;
	} else if (report->id == MSBC_AUDIO1_REPORT_ID) {
		/* first do special case check if there is any
		 * keyCode active in this report.  if so, we
		 * generate the same keyCode but on report 2, which
		 * is where normal keys are reported.  the keycode
		 * is being sent in the audio packet to save packets
		 * and over the air bandwidth.
		 */
		if (data[2] & KEYCODE_PRESENT_IN_AUDIO_PACKET_FLAG) {
			u8 key_data[3];
			key_data[0] = INPUT_REPORT_ID;
			key_data[1] = data[1]; /* low byte */
			key_data[2] = data[2]; /* high byte */
			key_data[2] &= ~KEYCODE_PRESENT_IN_AUDIO_PACKET_FLAG;
			hid_report_raw_event(hdev, 0, key_data,
					     sizeof(key_data), 0);
			pr_info("%s: generated hid keycode 0x%02x%02x\n",
				__func__, key_data[2], key_data[1]);
		}

		/* send the audio part to the alsa audio decoder for mSBC */
#if (DEBUG_AUDIO_RECEPTION == 1)
		if (packet_counter == 0) {
			snd_atvr_log("first MSBC packet received\n");
		}
#endif
		/* strip the one byte report id and two byte keycode field */
		audio_dec(&data[1 + 2], PACKET_TYPE_MSBC, size - 1 - 2);
		/* we've handled the event */
		return 1;
	} else if ((report->id == MSBC_AUDIO2_REPORT_ID) ||
		   (report->id == MSBC_AUDIO3_REPORT_ID)) {
		/* strip the one byte report id */
		audio_dec(&data[1], PACKET_TYPE_MSBC, size - 1);
		/* we've handled the event */
		return 1;
	}
	/* let the event through for regular input processing */
	return 0;
}

static int atvr_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;

	/* since vendor/product id filter doesn't work yet, because
	 * Bluedroid is unable to get the vendor/product id, we
	 * have to filter on name
	 */
	pr_info("%s: hdev->name = %s, vendor_id = %d, product_id = %d, num %d\n",
		__func__, hdev->name, hdev->vendor, hdev->product, num_remotes);
#if 0
	if (strcmp(hdev->name, "ADT-1_Remote") &&
	    strcmp(hdev->name, "Spike") &&
	    strcmp(hdev->name, "Nexus Remote")) {
		ret = -ENODEV;
		goto err_match;
	}
#endif
	pr_info("%s: Found target remote %s\n", __func__, hdev->name);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parse failed\n");
		goto err_parse;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		goto err_start;
	}

	// Lazy-creation of the soundcard, and enable the wired headset only then
	// to avoid race conditions on subsequent connections.
	// AudioService.java delays enabling the output
	if (!card_created) {
		ret = atvr_snd_initialize(hdev);
		if (ret)
			goto err_stop;
		card_created = true;
#ifdef WITH_SWITCH
		switch_set_state(&h2w_switch, BIT_HEADSET);
#endif
	}
	pr_info("%s: num_remotes %d->%d\n", __func__, num_remotes, num_remotes + 1);
	num_remotes++;

	return 0;
err_stop:
	hid_hw_stop(hdev);
err_start:
err_parse:
//err_match:
	return ret;
}

static void atvr_remove(struct hid_device *hdev)
{
	pr_info("%s: hdev->name = %s removed, num %d->%d\n",
		__func__, hdev->name, num_remotes, num_remotes - 1);
	num_remotes--;
	hid_hw_stop(hdev);
}

#if 1
//static const struct hid_device_id atvr_devices[] = {
static  struct hid_device_id atvr_devices[] = {
	{HID_DEVICE(0xAAAA,0xAAAA,0xAAAA,0xAAAA)},//warning!!! this is for insmod customized use only, and must be the first member variable
	{HID_DEVICE(5,3,10007,12976)},
	{HID_DEVICE(5,0,10007,12976)},
	{HID_DEVICE(5,1,10007,12976)},
	{HID_DEVICE(5,1,0,0)},
	{HID_DEVICE(5,1,6353,11330)},
//	{HID_DEVICE(3,1,257,7)},
	{HID_DEVICE(5,0,0x18d1,0x2c42)},
	{HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_GOOGLE,
			      USB_DEVICE_ID_ADT1_REMOTE)},
	{HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_GOOGLE,
			      USB_DEVICE_ID_NEXUS_REMOTE)},
	{HID_BLUETOOTH_DEVICE(0x248a,
	0x8266)},
	{HID_BLUETOOTH_DEVICE(0x248a,
	0x8267)},
	{ }
};
MODULE_DEVICE_TABLE(hid, atvr_devices);
static struct hid_driver atvr_driver;
#endif 

#if 0
static struct hid_driver atvr_driver = {
	.name = "AndroidTV remote",
	.id_table = atvr_devices,
	.raw_event = atvr_raw_event,
	.probe = atvr_probe,
	.remove = atvr_remove,
};
#endif 

static int atvr_init(void)
{
	int ret;

#if 1
atvr_devices[0].bus = dev_id[0];
atvr_devices[0].group = dev_id[1];
atvr_devices[0].vendor = dev_id[2];
atvr_devices[0].product = dev_id[3];
#endif

atvr_driver.name = "AndroidTV remote";
atvr_driver.id_table = atvr_devices;
atvr_driver.raw_event = atvr_raw_event;
atvr_driver.probe = atvr_probe;
atvr_driver.remove = atvr_remove;

printk("after:bus[0]:%d group[0]:%d  vendor[0]:%d product[0]:%d num:%lu\n", atvr_driver.id_table[0].bus, atvr_driver.id_table[0].group,  atvr_driver.id_table[0].vendor,\
	   																											atvr_driver.id_table[0].product, sizeof(atvr_devices)/sizeof(atvr_devices[0]));
#if 1//zewen
//#ifdef USE_8K_SAMPLE
if(8 == mic_sample_rate)
{
//8K
printk("zewen---> [FUNC]%s [LINE]:%d mic_sample_rate:%d You select 8K sample mode!!\n", __FUNCTION__, __LINE__, mic_sample_rate);
	USE_RATE_MIN = 8000;
	USE_RATE_MAX = 8000;
	//unsigned int USE_RATES_ARRAY = USE_RATE_MIN;
	USE_RATES_MASK = SNDRV_PCM_RATE_8000;

	MAX_FRAMES_PER_BUFFER = 8192;

	USE_PERIODS_MAX = 1024; // Ori 1024

	MAX_PCM_BUFFER_SIZE = (MAX_FRAMES_PER_BUFFER * sizeof(int16_t));
	MIN_PERIOD_SIZE = 64; //Ori64
	MAX_PERIOD_SIZE = (MAX_PCM_BUFFER_SIZE / 8);
}
else
{
//16K
printk("zewen-->[FUNC]%s [LINE]:%d mic_sample_rate:%d now is 16K sample mode, Include the following situation:\n1.default is 16K sample mode\n2.you do not select 8K mode\n", \
																														__FUNCTION__, __LINE__, mic_sample_rate);
	USE_RATE_MIN = 16000;
	USE_RATE_MAX = 16000;
//	#define USE_RATES_ARRAY      {USE_RATE_MIN}
	USE_RATES_MASK = SNDRV_PCM_RATE_16000;

	MAX_FRAMES_PER_BUFFER = (8192*2);

	USE_PERIODS_MAX = 2048; // Ori 1024

	MAX_PCM_BUFFER_SIZE = (MAX_FRAMES_PER_BUFFER * sizeof(int16_t));
	MIN_PERIOD_SIZE = 128; //Ori64
	MAX_PERIOD_SIZE = (MAX_PCM_BUFFER_SIZE / 8);

} //USE_8K_SAMPLE

#endif//zewen
MAX_PACKETS_PER_BUFFER = (MAX_FRAMES_PER_BUFFER / MIN_SAMPLES_PER_PACKET_P2);
MAX_BUFFER_SIZE = (MAX_PACKETS_PER_BUFFER * sizeof(struct fifo_packet));

atvr_pcm_hardware.rates = USE_RATES_MASK;
atvr_pcm_hardware.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_RESUME | SNDRV_PCM_INFO_MMAP_VALID);
atvr_pcm_hardware.formats = USE_FORMATS;
atvr_pcm_hardware.rates = USE_RATES_MASK;
atvr_pcm_hardware.rate_min = USE_RATE_MIN;
atvr_pcm_hardware.rate_max = USE_RATE_MAX;
atvr_pcm_hardware.channels_min = USE_CHANNELS_MIN;
atvr_pcm_hardware.channels_max = USE_CHANNELS_MAX;
atvr_pcm_hardware.buffer_bytes_max = MAX_PCM_BUFFER_SIZE;
atvr_pcm_hardware.period_bytes_min = MIN_PERIOD_SIZE;
atvr_pcm_hardware.period_bytes_max = MAX_PERIOD_SIZE;
atvr_pcm_hardware.periods_min =	USE_PERIODS_MIN;
atvr_pcm_hardware.periods_max =	USE_PERIODS_MAX;
atvr_pcm_hardware.fifo_size = 0;

#ifdef WITH_SWITCH
	ret = switch_dev_register(&h2w_switch);
	if (ret)
		pr_err("%s: failed to create h2w switch\n", __func__);
#endif

	ret = hid_register_driver(&atvr_driver);
	if (ret)
		pr_err("%s: can't register AndroidTV Remote driver\n", __func__);

#if (DEBUG_WITH_MISC_DEVICE == 1)
	pcm_dev_node.minor = MISC_DYNAMIC_MINOR;
	pcm_dev_node.name = "snd_atvr_pcm";
	pcm_dev_node.fops = &pcm_fops;
	ret = misc_register(&pcm_dev_node);
	if (ret)
		pr_err("%s: failed to create pcm misc device %d\n",
		       __func__, ret);
	else
		pr_info("%s: succeeded creating misc device %s\n",
			__func__, pcm_dev_node.name);

	adpcm_dev_node.minor = MISC_DYNAMIC_MINOR;
	adpcm_dev_node.name = "snd_atvr_adpcm";
	adpcm_dev_node.fops = &adpcm_fops;
	ret = misc_register(&adpcm_dev_node);
	if (ret)
		pr_err("%s: failed to create adpcm misc device %d\n",
		       __func__, ret);
	else
		pr_info("%s: succeeded creating misc device %s\n",
			__func__, adpcm_dev_node.name);

	mSBC_dev_node.minor = MISC_DYNAMIC_MINOR;
	mSBC_dev_node.name = "snd_atvr_mSBC";
	mSBC_dev_node.fops = &mSBC_fops;
	ret = misc_register(&mSBC_dev_node);
	if (ret)
		pr_err("%s: failed to create mSBC misc device %d\n",
		       __func__, ret);
	else
		pr_info("%s: succeeded creating misc device %s\n",
			__func__, mSBC_dev_node.name);
#endif

	return ret;
}

static void atvr_exit(void)
{
#if (DEBUG_WITH_MISC_DEVICE == 1)
	misc_deregister(&mSBC_dev_node);
	misc_deregister(&adpcm_dev_node);
	misc_deregister(&pcm_dev_node);
#endif

	hid_unregister_driver(&atvr_driver);

#ifdef WITH_SWITCH
	switch_set_state(&h2w_switch, BIT_NO_HEADSET);
	switch_dev_unregister(&h2w_switch);
#endif
	if(pcard)
        snd_card_free(pcard);
}

module_init(atvr_init);
module_exit(atvr_exit);
MODULE_LICENSE("GPL");


#ifdef TEST_ADPCM_DATA_FILL
void mic_to_adpcm_split (signed short *ps, int len, signed short *pds, int start)
{
    int i, j;
    unsigned short code=0;
    unsigned short code16=0;
    static signed short *pd;
    static int predict_idx = 0;
    static int predict = 0;
    
    const int *idxtbl = ima_index_table;
    const int16_t *steptbl = ima_step_table;
    
    int step = 0;
    int diff = 0;
    int diffq = 0;

#if 0
	int  iii = 0;
	unsigned char *pj = (unsigned char *)ps;
	if(start){
		printk("%x %x ", pj[0], pj[1]);
		iii += 2;
	}
	//for(; iii < len*2; iii++)
	for(; iii < 160; iii++)
	{
		printk("%x ", *(pj + iii));
	}
	printk("\n");
#endif 

	static int ii = 0;

    pd = pds;
    //byte2,byte1: predict;  byte3: predict_idx; byte4:adpcm data len
    if (start){
        predict_idx = 0;
        predict = ps[0];
        *(((signed char *)pd)) = predict&0xff;
        *(((signed char *)pd)+1 ) = (predict>>8)&0xff;
		
#if 1
		if(ii == 0)
		{
			 printk("miles---> [FUNC]:%s [LINE]:%d, predict:0x%x D:%d predict>>8:0x%x sizeof(hexData_10s):%d sizeof(short):%d \n", \
					 				__FUNCTION__, __LINE__, predict,predict, (predict>>8)&0xff, sizeof(hexData_10s), sizeof(short));
			ii++;
		}
#endif 
	//	printk("%x %x ", predict&0xff,  (predict>>8)&0xff);//local pcm, zewen
        len = len - 1;
        //*pd++ = ps[4];
        //ps++;
        pd++;
    }
signed char *outp;
outp = (signed char *)pd;
int outbuf;
int bufstep = 1;
    //byte5- byte128: 124 byte(62 sample) adpcm data
//#ifndef R_32K_DATA
    for (i=0; i<len; i++) {
//#else 
  //  for (i=0; i<len-1; i++) {
//#endif
        s16 di;
        if (start){
            // *2是因为其实一个sample占用4Bytes，但是我们只使用2个Byte，相当于取了MSB
            //di = ps[(i+4)*2];
#ifdef R_32K_DATA
            di = ps[i*2+2];
	//		printk("%x %x ", ps[i*2+2]&0xff, (ps[i*2+2]>>8)&0xff);//local pcm,zewen
#else 
            di = ps[i+1];
		//	printk("%x %x ", ps[i+1]&0xff, (ps[i+1]>>8)&0xff);//local pcm, zewen
#endif
        } else {
#ifndef R_32K_DATA
            di = ps[i];
		//	printk("%x %x ",  ps[i]&0xff, (ps[i]>>8)&0xff);//local pcm, zewen
#else
			di = ps[i*2];
		//	printk("%x %x ",  ps[i*2]&0xff, (ps[i*2]>>8)&0xff);//local pcm,zewen
#endif
        }

		//printk("di:%x ", di);
        step = steptbl[predict_idx];
        diff = di - predict;
        if (diff >=0 ) {
		//	if(ii < 100)
		//printk("%d %d \n",  __LINE__,diff);
            code = 0;
        } else {
		//	if(ii < 100)
		//printk("%d %d \n",  __LINE__,diff);
            diff = -diff;
            code = 8;
		//	if(ii < 100)
		//printk("%d %d \n",  __LINE__,diff);
        }
        diffq = step >> 3;
		//if(ii < 100)
		//printk("%d %d %d %d\n", diff,diffq,step,code);
        for (j=4; j>0; j=j>>1) {
            if( diff >= step) {
                diff = diff - step;
                diffq = diffq + step;
                code = code + j;
            }
            step = step >> 1;
		//	if(ii < 100)
		//	printk("%d %d %d %d\n", diff,diffq,step,code);
        }
		ii++;
#if 0
        if((i&3) == 0)
            code16 = (code&0x0f)<< 4;
        else if((i&3) ==1)
            code16 = code16 | (code&0x0f);
        else if((i&3) ==2)
            code16 = code16 | ((code&0x0f)<<12);
        else if ((i&3) == 3)
        {
            code16 = code16 | ((code&0x0f)<<8);
            *pd++ = code16;
        }
#endif

#if 1
		if(bufstep){
			outbuf = (code << 4) &0xf0;
		} else {
			*outp++ = (code &0xf) | outbuf;
		}
		bufstep = !bufstep;
#endif

#if 0
        code16 = (code16 >> 4) | (code << 12);
        if ( (i&3) == 3) {
            *pd++ = code16;
        }
#endif
        if(code >= 8) {
            predict = predict - diffq;
        } else {
            predict = predict + diffq;
        }
        if (predict > 32767) {
            predict = 32767;
        } else if (predict < -32768) {
            predict = -32768;
        }

        predict_idx = predict_idx + idxtbl[code];
        if(predict_idx < 0) {
            predict_idx = 0;
        } else if(predict_idx > 88) {
            predict_idx = 88;
        }
		//	if(ii < 100)
		//		printk("predict:%x predict_indx:%d\n", predict, predict_idx);
    }
	if(!bufstep)
		*outp++ = outbuf;
	//printk("\n");//local pcm, zewen
}
#endif


