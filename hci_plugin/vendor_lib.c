#include "bt_vendor_lib.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const bt_vendor_callbacks_t* mCbks;
static int mFd = -1;


static int op(bt_vendor_opcode_t opcode, void *param)
{
	int **fdsP = (int**)param;	//for "open"

	switch (opcode) {
		case BT_VND_OP_POWER_CTRL:
			//we're always on
			return 0;

		case BT_VND_OP_FW_CFG:
			//we're configured!
			mCbks->fwcfg_cb(BT_VND_OP_RESULT_SUCCESS);
			return 0;

		case BT_VND_OP_SCO_CFG:
			//we're configured!
			mCbks->scocfg_cb(BT_VND_OP_RESULT_SUCCESS);
			return 0;

		case BT_VND_OP_USERIAL_OPEN:
			if (mFd < 0)
				mFd = open(FILEPATH, O_RDWR);

			if (mFd >= 0) {
				fdsP[0][CH_CMD] = mFd;
				return 1;	//number of fds retruned
			}

			return 0;

		case BT_VND_OP_USERIAL_CLOSE:
			if (mFd >= 0)
				close(mFd);
			mFd = -1;
			return 0;

		case BT_VND_OP_GET_LPM_IDLE_TIMEOUT:
			//no such thing for us here
			return 0;

		case BT_VND_OP_LPM_SET_MODE:
			//we never fail!
			mCbks->lpm_cb(BT_VND_OP_RESULT_SUCCESS);
			return 1;

		case BT_VND_OP_LPM_WAKE_SET_STATE:
			//we have no states
			return 0;

		case BT_VND_OP_SET_AUDIO_STATE:
			//we have no states
			return 0;

		case BT_VND_OP_EPILOG:
			//we're always ready to shut down
			return 0;
	}

	return -1;
}

static int init(const bt_vendor_callbacks_t* cb, unsigned char *local_bdaddr)
{
	mCbks = cb;
	return 0;
}

static void cleanup(void)
{
	//nothing, but must exist
}

const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
	sizeof(bt_vendor_interface_t),
	init,
	op,
	cleanup
};
