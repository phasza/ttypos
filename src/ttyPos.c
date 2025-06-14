#include "ttyPos.h"

#define DRV_VERSION	"316"
#define VERSION_DATE    "2022.05.17_T"
#define MAX_RETRY_S	5
#define DRV_NAME	"ttyPos"

static struct tty_pos *pdx_table[POS_TTY_MINORS];

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
static struct tty_port	pos_port[POS_TTY_MINORS];
#endif

static unsigned char ResetPipePort(struct tty_pos *dev)
{
	return 0;
	struct tty_pos *pdx = dev;
	int retval;

	retval = usb_clear_halt(pdx->udev, usb_sndbulkpipe(pdx->udev,
	    pdx->bulk_out_epAddr));
	if (retval) {
		ERR("%s - ERROR CLEAR %X HALT = %d",
		    __func__, pdx->bulk_out_epAddr, retval);
		goto reset_port;
	}

	retval = usb_clear_halt(pdx->udev, usb_rcvbulkpipe(pdx->udev,
	    pdx->bulk_in_epAddr));
	if (retval) {
		ERR("%s - ERROR CLEAR %X HALT = %d",
		    __func__, pdx->bulk_in_epAddr, retval);
		goto reset_port;
	}

	return 0;

 reset_port:
	retval = usb_reset_device(pdx->udev);
	if (retval) {
		ERR("%s - ERROR RESETTING DEVICE: %d", __func__, retval);
	}

	return retval;
}

static int VerifyChecksum(ST_BULK_IO *p_bio)
{
	unsigned char a, b;
	int i, dn;

	dn = p_bio->Len + 4;
	a = 0;

	for (i = 2; i < dn; i++) {
		a ^= ((unsigned char *)p_bio)[i];
	}

	a ^= p_bio->SeqNo & 0x0f;
	a ^= p_bio->ReqType & 0x0f;
	b = (p_bio->SeqNo & 0xf0) + (p_bio->ReqType >> 4);
	if (a != b)
		return 1;

	/* clear checksum field */
	p_bio->SeqNo &= 0x0f;
	p_bio->ReqType &= 0x0f;
	return 0;
}

static void SetChecksum(ST_BULK_IO *p_bio)
{
	unsigned char a;
	int i, dn;

	dn = p_bio->Len + 4;
	a = 0;

	for (i = 2; i < dn; i++) {
		a ^= ((unsigned char *)p_bio)[i];
	}

	a ^= p_bio->SeqNo & 0x0f;
	a ^= p_bio->ReqType & 0x0f;

	/* fill high 4 bits of checksum into high 4 bits of ID field */
	p_bio->SeqNo = (p_bio->SeqNo & 0x0f) | (a & 0xf0);

	/* fill low 4 bits of checksum into high 4 bits of REQ_TYPE field */
	p_bio->ReqType |= (a << 4);
}

static unsigned char GetXOR(unsigned char *buf,unsigned int len)
{
	unsigned char a;
	unsigned int i;

	for(i=0,a=0;i<len;i++)
	  a^=buf[i];

	return a;
}

#if (LINUX_VERSION_CODE >KERNEL_VERSION(2,6,18))
static void UrbCallBack(struct urb *urb)
#else
static void UrbCallBack(struct urb *urb,struct pt_regs *regs)
#endif
{
	struct tty_pos *pdx;

	pdx = (struct tty_pos *)urb->context;

	atomic_set(&pdx->urb_done, 1);
	wake_up(&pdx->urb_wait);
}

static int SendAndWaitUrb(struct tty_pos *dev,unsigned char isOut,
    unsigned int pipe,unsigned char *buffer,unsigned int length,
    unsigned int *act_length)
{
	struct tty_pos *pdx = dev;
	int retval=0;
    volatile unsigned int actlen,tlen;

    actlen=0;
    *act_length = 0;
    while(1)
    {
        if(isOut)
        {
            if((length-actlen) > pdx->max_transfer_size)tlen = pdx->max_transfer_size;
            else tlen=length-actlen;
        }
        else tlen = pdx->max_transfer_size;
        
    	atomic_set(&pdx->urb_done, 0);
    	usb_fill_bulk_urb(pdx->urb, pdx->udev,pipe,buffer+actlen, tlen, UrbCallBack, pdx);

    	/* send the data out the bulk port */
    	retval = usb_submit_urb(pdx->urb, GFP_ATOMIC);
    	if (retval) {
    		ERR("%s - FAILED SUBMITTING WRITE URB: %d", __func__, retval);
    		retval = 1;
    		goto exit;
    	}

        retval = wait_event_timeout(pdx->urb_wait,
    	    (atomic_read(&pdx->urb_done) == 1), pdx->timeout_jiffies);
    	if (retval == 0)
        {
    		if (atomic_read(&pdx->urb_done) == 0)
            {
                usb_kill_urb(pdx->urb);
        		retval = 2;
                goto exit;
    		}
    	}
    	else if (retval < 0) 
        {
    		ERR("%s - WAIT FAILED: %d", __func__, retval);
    		retval = 3;
    		goto exit;
    	}

        actlen+=pdx->urb->actual_length;
        if(pdx->urb->actual_length!=tlen  || actlen>=length)
        {
            retval=0;
            *act_length = actlen;
            goto exit;
        }
    }
    
 exit:
	if (pdx->urb->status) 
    {
		/* if (pdx->urb->status != -EREMOTEIO) */
		{
			ERR("%s - error status: %d,done:%d length:%d,pipe:0x%X,isOut:%d", __func__,
			    pdx->urb->status,atomic_read(&pdx->urb_done),length,pipe,isOut);
            ERR("urb transfer_buffer_length:%d,actual_length:%d\n",pdx->urb->transfer_buffer_length,
                pdx->urb->actual_length);    
		}
	}
	return retval;
}

/* error codes: 11~29 */
static int ProcessCommand(struct tty_pos *dev)
{
	struct tty_pos *pdx = dev;
	int retval;
    unsigned int actlen;
    unsigned char reqType;
    unsigned char *buf;

	/* stage 1: send command pack */
    if(pdx->udev->descriptor.bcdDevice >= 0x300)
    {
        pdx->BioPack->Len += 1;
        pdx->BioPack->Data[pdx->BioPack->Len-1]=GetXOR((unsigned char *)pdx->BioPack, pdx->BioPack->Len-1+4);
    }
    else SetChecksum((ST_BULK_IO *)pdx->BioPack);

	if (pdx->urb == NULL)
		return 18;

	if (pdx->urb->status == -EINPROGRESS)
		return 19;

	retval = SendAndWaitUrb((struct tty_pos *)pdx,1,usb_sndbulkpipe(pdx->udev, pdx->bulk_out_epAddr),
        (unsigned char *)pdx->BioPack,pdx->BioPack->Len + 4,&actlen);

	if (retval != 0)
		return retval + 10;

    reqType = pdx->BioPack->ReqType;
	/* stage 2: receive answer pack */

	/* clear pack flags */
	pdx->BioPack->SeqNo = 0;
	pdx->BioPack->ReqType = 0;
	pdx->BioPack->Len = 0;
	if (pdx->urb == NULL)
		return 28;

    if(reqType!=READ_COMMAND)
    {
    	retval = SendAndWaitUrb((struct tty_pos *)pdx,0,usb_rcvbulkpipe(pdx->udev, pdx->bulk_in_epAddr),
            (unsigned char*)pdx->BioPack,pdx->max_transfer_size,&actlen);
    	if (retval != 0)
    		return retval + 20;
    }
    else
    {
        buf=(unsigned char*)pdx->BioPack;
    	retval = SendAndWaitUrb((struct tty_pos *)pdx,0,usb_rcvbulkpipe(pdx->udev, pdx->bulk_in_epAddr),
            buf,pdx->max_transfer_size,&actlen);
    	if (retval != 0)
    		return retval + 20;

        if((pdx->BioPack->Len+4) > pdx->max_transfer_size)
        {
        	retval = SendAndWaitUrb((struct tty_pos *)pdx,0,usb_rcvbulkpipe(pdx->udev, pdx->bulk_in_epAddr),
                buf+pdx->max_transfer_size,pdx->BioPack->Len+4-pdx->max_transfer_size,&actlen);
        	if (retval != 0)
        		return retval + 20;
        }
    }

    if(pdx->udev->descriptor.bcdDevice >= 0x300)
    {
        if(pdx->BioPack->Len<1)return 29;
        if(GetXOR((unsigned char *)pdx->BioPack, pdx->BioPack->Len+4))
        {
            ERR("VERIFY CHECKSUM FAILED: %d\n", retval);
    		ERR("%X; %X; %X\n", pdx->BioPack->SeqNo,
    		    pdx->BioPack->ReqType, pdx->BioPack->Len);
            return 29;
        }
        pdx->BioPack->Len -=1;
    }
    else
    {
    	if (VerifyChecksum((ST_BULK_IO *)pdx->BioPack)) 
        {
    		unsigned int i;
    		/* unsigned char x; */

    		ERR("VERIFY CHECKSUM FAILED: %d\n", retval);
    		ERR("%X; %X; %X\n", pdx->BioPack->SeqNo,
    		    pdx->BioPack->ReqType, pdx->BioPack->Len);

    		for (i = 0; i < 508; i++) {
    			INFO("%X\n", pdx->BioPack->Data[i]);
    		}
#if 0
    		for (i = 0, x = pdx->BioPack.Data[0];
    		    i < pdx->BioPack.Len; i++, x++) {
    			if (pdx->BioPack.Data[i] != x) {
    				printk(KERN_ALERT "%d: %X; %X\n", i - 1,
    				    pdx->BioPack.Data[i - 1], x - 1);
    				printk(KERN_ALERT "%d: %X; %X\n", i,
    				    pdx->BioPack.Data[i], x);
    				printk(KERN_ALERT "%d: %X; %X\n", i + 1,
    				    pdx->BioPack.Data[i + 1], x + 1);
    				printk(KERN_ALERT "%d: %X; %X\n", i + 2,
    				    pdx->BioPack.Data[i + 2], x + 2);
    				printk(KERN_ALERT "%d: %X; %X\n", i + 3,
    				    pdx->BioPack.Data[i + 3], x + 3);
    				break;
    			}
		}
#endif
		return 29;
	    }
    }

	return 0;
}

static void SleepMs(unsigned int nMs, struct tty_pos *dev)
{
	struct tty_pos *pdx = dev;

	unsigned int timeout = (nMs * HZ / 1000);
	if (timeout < 1) {
		timeout = 1;
	}

	wait_event_timeout(pdx->write_wait,
	    (atomic_read(&pdx->write_flag) == 1), timeout);
}

static int ThreadProcessing(void *data)
{
	struct tty_pos *pdx = data;
	unsigned char loops;
	int retval=0;
	unsigned int i, rlen, wlen;
	struct tty_struct *tty=NULL;
	unsigned long flags;

	if(pdx==NULL)
	{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,17,0))
		return -EINVAL;
#else
		do_exit(0);
#endif
	}
	
	tty = pdx->tty;
	if(tty==NULL)
	{
		retval = -EIO;
		goto exit;
	}

//RESTART:
	retval = ResetPipePort(pdx);
	if (retval != 0) {
		retval = 1;
		goto exit;
	}

    if(pdx->udev->descriptor.bcdDevice >= 0x300)
    {
        retval =usb_control_msg(pdx->udev,usb_sndctrlpipe(pdx->udev, 0),
            0x01,0x40,0x300,0,
            0,0,5000);
        if(retval <0)
        {
            ERR("set host error :%d\n",retval);
            retval = -EPIPE;
            goto exit;
        }
    }

    for(loops=0;pdx->udev->descriptor.bcdDevice >= 0x200 && loops<MAX_RETRY_S;loops++)
    {
		if(!loops)pdx->SeqCount = (pdx->SeqCount + 1) & 0x0f;
		pdx->BioPack->SeqNo = pdx->SeqCount;
		pdx->BioPack->ReqType = MAXDATA_COMMAND;
		pdx->BioPack->Len = 0;
        retval = ProcessCommand((struct tty_pos *)pdx);

        if(retval!=0)
        {
            retval+=400;
            goto loop_g_tail;
        }
        if(pdx->BioPack->Len !=2)
        {
            retval=430;
            goto loop_g_tail;
        }
		if (pdx->BioPack->SeqNo != pdx->SeqCount) 
        {
			retval = 431;
			goto loop_g_tail;
		}

		if (pdx->BioPack->ReqType != MAXDATA_COMMAND) 
        {
			retval = 432;
			goto loop_g_tail;
		}

        if(pdx->BioPack->Len==0)
        {
            retval = 433;
            goto loop_g_tail;
        }

        memcpy(&pdx->maxdata,pdx->BioPack->Data,sizeof(pdx->maxdata));
        if(pdx->maxdata>sizeof(pdx->BioPack->Data))
            pdx->maxdata = sizeof(pdx->BioPack->Data);

        INFO("maxdata:%d\n",pdx->maxdata);
 loop_g_tail:
		if (retval == 0)
			break;
		ERR("GET_DATA_INFO RETRY, loop: %d, err: %d, seq: %02X\n",
		    loops, retval, pdx->SeqCount);
		ResetPipePort(pdx);        
    }
    if(retval)goto exit;

	while (pdx->ThreadState == THREAD_RUNNING) {

		/* get device buffer status */
		for (loops = 0; (loops < MAX_RETRY_S) &&
		    (pdx->ThreadState == THREAD_RUNNING); loops++) {
			/* building command pack */
			if(!loops)pdx->SeqCount = (pdx->SeqCount + 1) & 0x0f;
			pdx->BioPack->SeqNo = pdx->SeqCount;
			pdx->BioPack->ReqType = STATUS_COMMAND;
			pdx->BioPack->Len = 0;

			retval = ProcessCommand((struct tty_pos *)pdx);
			if (retval != 0) {
				retval += 100;
				goto loop_s_tail;
			}

			if (pdx->BioPack->Len != 16) {
                ERR("pdx->BioPack->Len:%d\n",pdx->BioPack->Len);
				retval = 130;
				goto loop_s_tail;
			}

			if (pdx->BioPack->SeqNo != pdx->SeqCount) {
				retval = 131;
				goto loop_s_tail;
			}

			if (pdx->BioPack->ReqType != STATUS_COMMAND) {
				retval = 132;
				goto loop_s_tail;
			}

			memcpy(&pdx->BioDevState, pdx->BioPack->Data,
			    sizeof(pdx->BioDevState));

			if ((!pdx->BioDevState.TxLeft) &&
			    IS_POOL_EMPTY(pdx->TxPool)) {
				SleepMs(1, (struct tty_pos *)pdx);
			}

 loop_s_tail:
			if (retval == 0)
				break;
			ERR("STATUS RETRY, loop: %d, err: %d, seq: %02X\n",
			    loops, retval, pdx->SeqCount);
			ResetPipePort(pdx);
		}

		if (retval != 0)
			goto exit;

 r_process:	/* read from usb device */

		for (loops = 0; (loops < MAX_RETRY_S) &&
		    (pdx->ThreadState == THREAD_RUNNING); loops++) {
			if (!pdx->BioDevState.TxLeft)
				goto w_process;

			rlen = pdx->BioDevState.TxLeft;

			if (rlen > (pdx->maxdata-1)) {
				rlen = pdx->maxdata-1;
			}
            //tmpd=rlen;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
            rlen = tty_buffer_request_room(&pos_port[pdx->devIndex],rlen);
#else
			rlen = tty_buffer_request_room(tty, rlen);
#endif
//            if(tmpd>rlen)
  //          {
    //            INFO("wantlen:%d,but len:%d..........\n",tmpd,rlen);
      //      }
            if(rlen==0)goto w_process;
			if (!loops) {
				pdx->SeqCount = (pdx->SeqCount + 1) & 0x0f;
			}

			pdx->BioPack->SeqNo = pdx->SeqCount;
			pdx->BioPack->ReqType = READ_COMMAND;
			pdx->BioPack->Len = 2;
			/* in dlen required */
			pdx->BioPack->Data[0] = (unsigned short)rlen & 0xff;
			pdx->BioPack->Data[1] = (unsigned short)rlen >> 8;

			retval = ProcessCommand((struct tty_pos *)pdx);
			if (retval != 0) {
				retval += 200;
				goto loop_r_tail;
			}

			if (pdx->BioPack->SeqNo != pdx->SeqCount) {
				retval = 231;
				goto loop_r_tail;
			}

			if (pdx->BioPack->ReqType != READ_COMMAND) {
				if ((pdx->BioPack->ReqType == STATUS_COMMAND) &&
				    (pdx->BioPack->Len >=
				    sizeof(pdx->BioDevState))) {
					memcpy(&pdx->BioDevState,
					    pdx->BioPack->Data,
					    sizeof(pdx->BioDevState));
					goto w_process;	/* no data to fetch */
				}

				retval = 232;
				ERR("  %02X, ERROR req_type: %02X.\n",
				    pdx->SeqCount, pdx->BioPack->ReqType);

				goto loop_r_tail;
			}

			if (pdx->BioPack->Len > rlen) {
				ERR("MORE DATA FETCHED THAN DECLARED, NEED: "
				    "%d, RN: %d\n", rlen, pdx->BioPack->Len);
				retval = 234;
				goto exit;
			}

			rlen = pdx->BioPack->Len;
#if 0
			for (j = 0; j < rlen - 1; j++) {
				if ((pdx->BioPack.Data[j] + 1 !=
				    pdx->BioPack.Data[j + 1])
				    && (pdx->BioPack.Data[j + 1] != 0)) {
					inerr = 235;
					goto exit;
				}
			}
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
            tty_insert_flip_string(&pos_port[pdx->devIndex], pdx->BioPack->Data, rlen);
			tty_flip_buffer_push(&pos_port[pdx->devIndex]);
#else            
			tty_insert_flip_string(tty, pdx->BioPack->Data, rlen);
			tty_flip_buffer_push(tty);
#endif
			pdx->BioDevState.TxLeft -= rlen;
#if 0
			INFO("%02X, RN: %d\n", pdx->SeqCount,rlen);
#endif

			if (pdx->BioDevState.TxLeft)
				goto r_process;
 loop_r_tail:
			if (retval == 0)
				break;

			ERR("RX RETRY, loop: %d, err: %d, SEQ: %02X\n",
			    loops, retval, pdx->SeqCount);

			ResetPipePort(pdx);
		}

		if (retval)
			goto exit;

 w_process:	/* write to usb device */

		wlen = GET_USING_POOL(pdx->TxPool);
		if(wlen==0)
		{
			tty_wakeup(tty);
			continue;
		}
	
		if (wlen > (pdx->maxdata-1)) {
			wlen = pdx->maxdata-1;
		}

		if (wlen > pdx->BioDevState.RxLeft) {
			wlen = pdx->BioDevState.RxLeft;
		}

		for (loops = 0; (loops < MAX_RETRY_S) &&
		    (pdx->ThreadState == THREAD_RUNNING); loops++) {
			if (wlen == 0)
				break;

			if (!loops) {
				pdx->SeqCount = (pdx->SeqCount + 1) & 0x0f;
			}

			pdx->BioPack->SeqNo = pdx->SeqCount;
			pdx->BioPack->ReqType = WRITE_COMMAND;
			pdx->BioPack->Len = (unsigned short)wlen;

			for (i = 0; i < wlen; i++) {
				pdx->BioPack->Data[i] =
				    pdx->TxPool.Buffer[(pdx->TxPool.ReadPos +
				    i) % POOL_SIZE];
			}

			retval = ProcessCommand((struct tty_pos *)pdx);
			if (retval != 0) {
				retval += 300;
				goto loop_w_tail;
			}

			if (pdx->BioPack->Len != 16) {
				retval = 330;
				goto loop_w_tail;
			}

			if (pdx->BioPack->SeqNo != pdx->SeqCount) {
				retval = 331;

				ERR("***** Mismatched with SEQ: %02X, "
				    "wlen: %d, loop: %d\n", pdx->SeqCount,
				    wlen, loops);

				ERR(" SEQ: %02X, type: %02X, dn: %d\n",
				    pdx->BioPack->SeqNo, pdx->BioPack->ReqType,
				    pdx->BioPack->Len);

				for (i = 0; i < pdx->BioPack->Len; i++) {
					INFO("%02X ", pdx->BioPack->Data[i]);
				}

				if (((pdx->BioPack->SeqNo + 1) % 16) ==
				    pdx->SeqCount)
					goto loop_w_tail;

				goto exit;
			}	/* mismatched seq_no */

			if (pdx->BioPack->ReqType != STATUS_COMMAND) {
				retval = 332;

				ERR(" SEQ: %02X, type: %02X, dn: %d\n",
				    pdx->BioPack->SeqNo, pdx->BioPack->ReqType,
				    pdx->BioPack->Len);

				for (i = 0; i < sizeof(pdx->BioPack->Data);
				    i++) {
					INFO("%02X ", pdx->BioPack->Data[i]);
				}

				INFO("\n");

				goto exit;
			}	/* mismatched req_type */
#if 0
			INFO("%02X, WN: %d\n", pdx->SeqCount,wlen);
#endif
			local_irq_save(flags);
            if(pdx->TxPool.ReadPos!=pdx->TxPool.WritePos)
    			pdx->TxPool.ReadPos = (pdx->TxPool.ReadPos + wlen) %POOL_SIZE;
			local_irq_restore(flags);
			memcpy(&pdx->BioDevState, pdx->BioPack->Data,
			    sizeof(pdx->BioDevState));

			tty_wakeup(tty);
			if (!IS_POOL_EMPTY(pdx->TxPool))
				goto w_process;

			atomic_set(&pdx->write_flag, 0);
 loop_w_tail:
			if (retval == 0)
				break;

			ERR("TX RETRY, loop: %d, err: %d, SEQ: %02X\n",
			    loops, retval, pdx->SeqCount);

			ResetPipePort(pdx);
		}

		if (retval)
			goto exit;
	}

 exit:
	if ((retval != 0) && (atomic_read(&pdx->discon)==0)) {
		ERR("%s %02X, ERR: %d\n",__func__, pdx->SeqCount, retval);

		ResetPipePort(pdx);
	}

    atomic_set(&pdx->openCnt,0);

	if(tty!=NULL)
	{
		local_irq_save(flags);
		INIT_POOL_BUFFER(pdx->TxPool);
		local_irq_restore(flags);

		for(i=0;i<10;i++)
		{
			tty_wakeup(tty);
			msleep(1);
		}
	}

    local_irq_save(flags);
	pdx->ThreadState = THREAD_INIT;
    local_irq_restore(flags);
	INFO("ThreadProcessing Exit\n");
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,17,0))
	return 0;
#else
	do_exit(0);
#endif
}

static void pos_delete(struct kref *kref)
{
	struct tty_pos *pdx = to_pos_dev(kref);

	if (pdx == NULL)
		return;

    if(pdx->devIndex >= POS_TTY_MINORS)
		return;

    while(atomic_read(&pdx->rc_busy)==1)
        msleep(100);

	if (pdx->tty) {
		pdx->tty->driver_data = NULL;
	}

	pdx_table[pdx->devIndex] = NULL;
	usb_free_urb(pdx->urb);
	usb_put_dev(pdx->udev);
	kfree(pdx->BioPack);
	kfree(pdx);
}

static int pos_open(struct tty_struct *tty, struct file *filp)
{
	struct tty_pos *pdx;
    volatile unsigned int cnt=0;
    int ret=0;
	unsigned long flags;

    INFO("%s,tty:%p,filp:%p\n",__func__,tty,filp);
    
    if(tty==NULL)return -EINVAL;
    if(tty->index >= POS_TTY_MINORS || tty->index<0)return -ECHRNG;
    
	pdx = pdx_table[tty->index];
	if (pdx == NULL)
		return -ENODEV;

    if(pdx->interface==NULL || atomic_read(&pdx->discon))
        return -EIO;

	local_irq_save(flags);
    if(THREAD_IS_RUNNING(pdx->ThreadState))
	{
		atomic_inc(&pdx->openCnt);
		local_irq_restore(flags);
		return 0;
	}
	local_irq_restore(flags);
	
    atomic_set(&pdx->rc_busy,1);

	tty->driver_data = pdx;
	pdx->tty = tty;

    cnt = 0;
    while(pdx->ThreadState!=THREAD_INIT)
    {
        msleep(100);
        if(cnt++>50)
        {
            ret = -EBUSY;
            ERR("DEVICE BUSY state:0x%X\n",pdx->ThreadState);
            goto exit;
        }
        if(atomic_read(&pdx->discon)==1)
        {
            ret = -EIO;
            goto exit;
        }
    }

    #if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
    tty_port_tty_set(&pos_port[pdx->devIndex], tty);
    #endif

    local_irq_save(flags);
	pdx->ThreadState = THREAD_RUNNING;
    local_irq_restore(flags);
    {
        struct task_struct *thread;
        thread = kthread_run(ThreadProcessing,(struct tty_pos *)pdx,"ThreadProcessing");
        if (IS_ERR(thread))
        {
            local_irq_save(flags);
    		pdx->ThreadState = THREAD_INIT;
            local_irq_restore(flags);
    		ret = -ESRCH;
    		ERR("FAILED TO CREATE KERNEL THREAD!\n");
            goto exit;
    	}
    }
    atomic_inc(&pdx->openCnt);

exit:
    atomic_set(&pdx->rc_busy,0);
	return ret;
}

static void pos_close(struct tty_struct *tty, struct file *filp)
{
	struct tty_pos *pdx;
	int i;
	unsigned long flags;

    INFO("%s\n",__func__);

	if(tty==NULL)return;
	
	pdx = tty->driver_data;
	if (pdx == NULL)return;
    if(pdx_table[pdx->devIndex]==NULL)
        return;
    if(atomic_read(&pdx->openCnt)==0)return;
    atomic_set(&pdx->rc_busy,1);
    atomic_dec(&pdx->openCnt);
	if(atomic_read(&pdx->openCnt))
	{
        atomic_set(&pdx->rc_busy,0);
        return;
	}

	for(i=0;i<10;i++)
	{
		if(pdx->ThreadState != THREAD_RUNNING)break;
		if(pdx->TxPool.ReadPos==pdx->TxPool.WritePos)break;
		msleep(50);
	}
	
	local_irq_save(flags);
    if(THREAD_IS_RUNNING(pdx->ThreadState))
    {
        pdx->ThreadState=THREAD_STOPPED;
		local_irq_restore(flags);
		while(pdx->ThreadState==THREAD_STOPPED)
		{
			msleep(1);
		}
    }
    else
    {
        INIT_POOL_BUFFER(pdx->TxPool);
		local_irq_restore(flags);

		for(i=0;i<3;i++)
		{
			tty_wakeup(tty);
			msleep(1);
		}
    }
    	
    atomic_set(&pdx->rc_busy,0);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static ssize_t pos_write(struct tty_struct *tty, const u8 *buf, size_t count)
#else
static int pos_write(struct tty_struct *tty, const unsigned char *buf, int count)
#endif
{
	struct tty_pos *pdx = tty->driver_data;
	unsigned int wn, i;
	int retval;
	unsigned long flags;

	if (!pdx)
		return -ENODEV;

    if(pdx->interface==NULL || atomic_read(&pdx->discon))
        return -EIO;

	if (!THREAD_IS_RUNNING(pdx->ThreadState)) {
		retval = -EIO;
		goto exit;
	}

	if (count == 0) {
		retval = 0;
		goto exit;
	}

	local_irq_save(flags);
	wn = GET_SPACE_POOL(pdx->TxPool);
	if (wn >= count) {
		wn = count;
	}
    else if(wn == 0)
	{
		local_irq_restore(flags);
		retval = 0;
        goto exit;
	}

	for (i = 0; i < wn; i++) {
		pdx->TxPool.Buffer[(pdx->TxPool.WritePos + i) % POOL_SIZE] =
		    buf[i];
	}

	pdx->TxPool.WritePos = (pdx->TxPool.WritePos + wn) % POOL_SIZE;
	retval = wn;
    local_irq_restore(flags);

	atomic_set(&pdx->write_flag, 1);
	wake_up(&pdx->write_wait);
 exit:
	return retval;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0))
static unsigned int pos_write_room(struct tty_struct *tty)
#else
static int pos_write_room(struct tty_struct *tty)
#endif
{
	struct tty_pos *pdx = tty->driver_data;
	int room = -EINVAL;
	unsigned long flags;

	if (!pdx)return 0;
	local_irq_save(flags);
	room = GET_SPACE_POOL(pdx->TxPool);
	local_irq_restore(flags);
	return room;
}
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,0,0)
static int pos_ioctl(struct tty_struct *tty, struct file *filp,
    unsigned int cmd, unsigned long arg)
#else
static int pos_ioctl(struct tty_struct *tty, unsigned int cmd,
    unsigned long arg)
#endif    
{
	struct tty_pos *pdx = tty->driver_data;

	if (!pdx)
		return -ENODEV;

	switch (cmd) {
	case TIOCGSERIAL:
	case TIOCMIWAIT:
	case TIOCGICOUNT:
	default:
		break;
	}
	return -ENOIOCTLCMD;
}

#define RELEVANT_IFLAG(iflag) \
	((iflag) & (IGNBRK | BRKINT | IGNPAR | PARMRK | INPCK))

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static void pos_set_termios(struct tty_struct *tty, const struct ktermios *old_termios)
#elif LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 18)
static void pos_set_termios(struct tty_struct *tty, struct ktermios *old_termios)
#else
static void pos_set_termios(struct tty_struct *tty, struct termios *old_termios)
#endif
{
	unsigned int cflag;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
    cflag = tty->termios.c_cflag;
#else
	cflag = tty->termios->c_cflag;
#endif
	/* check that they really want us to change something */
	if (old_termios) {
		if ((cflag == old_termios->c_cflag) &&
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
            (RELEVANT_IFLAG(tty->termios.c_iflag) ==
#else            
		    (RELEVANT_IFLAG(tty->termios->c_iflag) ==
#endif
		    RELEVANT_IFLAG(old_termios->c_iflag))) {
#if 0
			INFO(" - nothing to change...\n");
#endif
			return;
		}
	}
#if 0
	/* get the byte size */
	switch (cflag & CSIZE) {
	case CS5:
		INFO(" - data bits = 5\n");
		break;
	case CS6:
		INFO(" - data bits = 6\n");
		break;
	case CS7:
		INFO(" - data bits = 7\n");
		break;
	default:
	case CS8:
		INFO(" - data bits = 8\n");
		break;
	}

	/* determine the parity */
	if (cflag & PARENB) {
		if (cflag & PARODD) {
			INFO(" - parity = odd\n");
		}
		else {
			INFO(" - parity = even\n");
		}
	}
	else {
		INFO(" - parity = none\n");
	}

	/* figure out the stop bits requested */
	if (cflag & CSTOPB) {
		INFO(" - stop bits = 2\n");
	}
	else {
		INFO(" - stop bits = 1\n");
	}

	/* figure out the hardware flow control settings */
	if (cflag & CRTSCTS) {
		INFO(" - RTS/CTS is enabled\n");
	}
	else {
		INFO(" - RTS/CTS is disabled\n");
	}

	/* determine software flow control.
	 * if we are implementing XON/XOFF, set the start and
	 * stop character in the device */
	if (I_IXOFF(tty) || I_IXON(tty)) {
		unsigned char stop_char = STOP_CHAR(tty);
		unsigned char start_char = START_CHAR(tty);

		/* if we are implementing INBOUND XON/XOFF */
		if (I_IXOFF(tty)) {
			INFO(" - INBOUND XON/XOFF is enabled, "
			    "XON = %2x, XOFF = %2x", start_char, stop_char);
		}
		else {
			INFO(" - INBOUND XON/XOFF is disabled");
		}

		/* if we are implementing OUTBOUND XON/XOFF */
		if (I_IXON(tty)) {
			INFO(" - OUTBOUND XON/XOFF is enabled, "
			    "XON = %2x, XOFF = %2x", start_char, stop_char);
		}
		else {
			INFO(" - OUTBOUND XON/XOFF is disabled");
		}
	}

	/* get the baud rate wanted */
	INFO(" - baud rate = %d\n", tty_get_baud_rate(tty));
#endif
}

static void pos_throttle(struct tty_struct *tty)
{
	/* INFO("pos_throttle\n"); */
}

static void pos_unthrottle(struct tty_struct *tty)
{
	/* INFO("pos_unthrottle\n"); */
}

static void pos_flush_buffer(struct tty_struct *tty)
{
	struct tty_pos *pdx;
	unsigned long flags;

	if(tty==NULL)return;
	pdx = tty->driver_data;
	if (!pdx)return;

	local_irq_save(flags);
	INIT_POOL_BUFFER(pdx->TxPool);
	local_irq_restore(flags);
	tty_wakeup(tty);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0))
static unsigned int pos_chars_in_buffer(struct tty_struct *tty)
#else
static int pos_chars_in_buffer(struct tty_struct *tty)
#endif
{
	int in_buf_len;
	struct tty_pos *pdx;
	unsigned long flags;

	if(tty==NULL)return 0;
	pdx = tty->driver_data;
	if (!pdx)return 0;
	if (atomic_read(&pdx->discon))return 0;
	
	local_irq_save(flags);
	in_buf_len = GET_USING_POOL(pdx->TxPool);
	local_irq_restore(flags);
	return in_buf_len;
}

/* Our fake UART values */
#define MCR_DTR		0x01
#define MCR_RTS		0x02
#define MCR_LOOP	0x04
#define MSR_CTS		0x08
#define MSR_CD		0x10
#define MSR_RI		0x20
#define MSR_DSR		0x40

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,0,0)
static int pos_tiocmget(struct tty_struct *tty, struct file *filp)
#else
static int pos_tiocmget(struct tty_struct *tty)
#endif
{
	struct tty_pos *pdx = tty->driver_data;
	unsigned int msr, mcr, result;
	unsigned long flags;

	if (!pdx)
		return -ENODEV;
    local_irq_save(flags);
	msr = pdx->msr;
	mcr = pdx->mcr;

	result = ((mcr & MCR_DTR) ? TIOCM_DTR : 0) |	/* DTR is set */
	    ((mcr & MCR_RTS) ? TIOCM_RTS : 0) |		/* RTS is set */
	    ((mcr & MCR_LOOP) ? TIOCM_LOOP : 0) |	/* LOOP is set */
	    ((msr & MSR_CTS) ? TIOCM_CTS : 0) |		/* CTS is set */
	    ((msr & MSR_CD) ? TIOCM_CAR : 0) |	/* Carrier detect is set */
	    ((msr & MSR_RI) ? TIOCM_RI : 0) |	/* Ring Indicator is set */
	    ((msr & MSR_DSR) ? TIOCM_DSR : 0);		/* DSR is set */
    local_irq_restore(flags);
	return result;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,0,0)
static int pos_tiocmset(struct tty_struct *tty, struct file *filp,
    unsigned int set, unsigned int clear)
#else
static int pos_tiocmset(struct tty_struct *tty, unsigned int set, 
    unsigned int clear)
#endif    
{
	struct tty_pos *pdx = tty->driver_data;
	unsigned int mcr;
	unsigned long flags;

	if (!pdx)
		return -ENODEV;
	local_irq_save(flags);
	mcr = pdx->mcr;

	if (set & TIOCM_RTS) {
		mcr |= MCR_RTS;
	}
	if (set & TIOCM_DTR) {
		mcr |= MCR_DTR;		/* mcr |= MCR_RTS; */
	}

	if (clear & TIOCM_RTS) {
		mcr &= ~MCR_RTS;
	}
	if (clear & TIOCM_DTR) {
		mcr &= ~MCR_DTR;	/* mcr &= ~MCR_RTS; */
	}

	/* set the new MCR value in the device */
	pdx->mcr = mcr;
    local_irq_restore(flags);
	return 0;
}

static const struct tty_operations pos_ops = {
	.open = pos_open,
	.close = pos_close,
	.write = pos_write,
	.write_room = pos_write_room,
	.ioctl = pos_ioctl,
	.set_termios = pos_set_termios,
	.throttle = pos_throttle,
	.unthrottle = pos_unthrottle,
	.flush_buffer = pos_flush_buffer,
	.chars_in_buffer = pos_chars_in_buffer,
	.tiocmget = pos_tiocmget,
	.tiocmset = pos_tiocmset,
};

struct tty_driver *pos_tty_driver;

static int pos_usb_probe(struct usb_interface *interface,
    const struct usb_device_id *id)
{
	struct tty_pos *pdx;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;

	int i, retval = -ENOMEM,errcnt;

    for(i=0;i<POS_TTY_MINORS;i++)
    {
		if (pdx_table[i] == NULL)
			break;
	}

    if(i==POS_TTY_MINORS)
		return -ENOMEM;

	for(errcnt=0;errcnt<5;errcnt++)
	{
		pdx = kzalloc(sizeof(*pdx),/* GFP_KERNEL */ GFP_ATOMIC);
		if(pdx)break;
		msleep(1);
	}
	if (!pdx) {
		ERR("OUT OF MEMORY pdx\n");
		return -ENOMEM;
	}

	for(errcnt=0;errcnt<5;errcnt++)
	{
		pdx->BioPack = kzalloc(sizeof(*pdx->BioPack), GFP_ATOMIC);
		if(pdx->BioPack)break;
		msleep(1);
	}
	if (!pdx->BioPack) {
		ERR("OUT OF MEMORY BioPack\n");
		return -ENOMEM;
	}


	INFO("ttyPos probe: %s %s, index:%d,PAGE_SIZE:%d\n",DRV_VERSION,VERSION_DATE,i,(unsigned int)PAGE_SIZE);

    atomic_set(&pdx->openCnt,0);
	pdx->devIndex = i;
	pdx_table[pdx->devIndex] = pdx;
    pdx->maxdata=508;
    pdx->ThreadState = THREAD_INIT;
	pdx->max_transfer_size = 512;

	INIT_POOL_BUFFER(pdx->TxPool);

	pdx->timeout_jiffies = 1000 * HZ / 1000;		/* 1000ms */
	kref_init(&pdx->kref);
	init_waitqueue_head(&pdx->urb_wait);
	atomic_set(&pdx->urb_done, 0);

	init_waitqueue_head(&pdx->write_wait);
	atomic_set(&pdx->write_flag, 0);

    atomic_set(&pdx->rc_busy,0);
    atomic_set(&pdx->discon,0);

	pdx->udev = usb_get_dev(interface_to_usbdev(interface));
	pdx->interface = interface;
    INFO("bcdDevice:%X\n",pdx->udev->descriptor.bcdDevice);
	pdx->urb = usb_alloc_urb(0, /* GFP_KERNEL */ GFP_ATOMIC);
	if (!pdx->urb) {
		retval = -ENOMEM;
		ERR("FAILED ALLOC URB!\n");
		goto error;
	}

	iface_desc = interface->cur_altsetting;

	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (endpoint->bEndpointAddress & 0x80) {
			pdx->bulk_in_epAddr = endpoint->bEndpointAddress;
			INFO("in_ep: wMaxPacketSize = %d\n", endpoint->wMaxPacketSize);

			if(endpoint->wMaxPacketSize == 64 && pdx->udev->speed == USB_SPEED_HIGH)
				pdx->max_transfer_size = 64;
		}
		else {
			pdx->bulk_out_epAddr = endpoint->bEndpointAddress;
			INFO("out_ep: wMaxPacketSize = %d\n", endpoint->wMaxPacketSize);
		}
	}

    if(pdx->udev->descriptor.bcdDevice >= 0x300)
    {
        retval =usb_control_msg(pdx->udev,usb_sndctrlpipe(pdx->udev, 0),
            0x01,0x40,0x300,0,
            0,0,1000);
        if(retval <0)
        {
            ERR("set host error :%d\n",retval);
            retval = -EPIPE;
            goto error;
        }
    }
    
	usb_set_intfdata(interface, pdx);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
    tty_port_register_device(&pos_port[pdx->devIndex], pos_tty_driver,
			pdx->devIndex, NULL);
#else
    tty_register_device(pos_tty_driver, pdx->devIndex, NULL);
#endif

    dev_info(&interface->dev,
                "USB POS device now attached to ttyPos%d",
                /*interface->minor*/pdx->devIndex);
	return 0;

 error:
	if (pdx) {
		kref_put(&pdx->kref, pos_delete);
	}

	ERR("--pos_probe error\n");
	return retval;
}

static void pos_usb_disconnect(struct usb_interface *interface)
{
	struct tty_pos *pdx;
	unsigned long flags;

    INFO("%s entry\n",__func__);

	pdx = usb_get_intfdata(interface);
	if (pdx == NULL)return;

    atomic_set(&pdx->discon,1);

	local_irq_save(flags);
    if(THREAD_IS_RUNNING(pdx->ThreadState))
    {
        pdx->ThreadState=THREAD_STOPPED;
		local_irq_restore(flags);
		while(pdx->ThreadState==THREAD_STOPPED)
		{
			msleep(1);
		}
    }
    else
    {
		local_irq_restore(flags);
    }
    	
	tty_unregister_device(pos_tty_driver, pdx->devIndex);
	#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
	tty_port_destroy(&pos_port[pdx->devIndex]);
	#endif

	usb_set_intfdata(interface, NULL);
	pdx->interface = NULL;

	kref_put(&pdx->kref, pos_delete);
    
    INFO("%s exit\n",__func__);
}

static int pos_usb_suspend(struct usb_interface *interface, pm_message_t message)
{
	struct tty_pos *pdx;

	INFO("%s \n",__func__);
	pdx = usb_get_intfdata(interface);
	if (pdx == NULL)return 0;
    
    if(THREAD_IS_RUNNING(pdx->ThreadState))return -EBUSY;

	return 0;
}

static int pos_usb_resume(struct usb_interface *interface)
{
	INFO("%s \n",__func__);
	
	return 0;
}

#ifdef OLD_USB_DRIVER
static void pos_usb_pre_reset(struct usb_interface *intf)
{
	/* struct tty_pos *pdx = usb_get_intfdata(intf); */
}
static void pos_usb_post_reset(struct usb_interface *intf)
{
}
#else
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18))
static int pos_usb_pre_reset(struct usb_interface *intf)
{
	/* struct tty_pos *pdx = usb_get_intfdata(intf); */
    INFO("%s\n",__func__);
	return 0;
}
static int pos_usb_post_reset(struct usb_interface *intf)
{
    INFO("%s\n",__func__);
	return 0;
}
#endif
#endif


static struct usb_driver pos_usb_driver = {
    .name   =       "ttyPos",
	.probe = pos_usb_probe,
	.disconnect = pos_usb_disconnect,
	.suspend = pos_usb_suspend,
	.resume = pos_usb_resume,
	#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18))
	.supports_autosuspend = 0,
	.pre_reset = pos_usb_pre_reset,
	.post_reset = pos_usb_post_reset,
    .reset_resume = pos_usb_resume,
	#endif
	.id_table = pos_usb_table,
};

/* Compatible with TTY_DRIVER_DYNAMIC_DEV and TTY_DRIVER_NO_DEVFS */
#define TTY_USB_DEV     0x0008 

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))

static int pos_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	return 0;
}

static void pos_port_shutdown(struct tty_port *port)
{

}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static bool pos_carrier_raised(struct tty_port *port)
#else
static int pos_carrier_raised(struct tty_port *port)
#endif
{
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static void pos_dtr_rts(struct tty_port *port, bool on)
#else
static void pos_dtr_rts(struct tty_port *port, int on)
#endif
{
}

static const struct tty_port_operations pos_port_ops = {
	.activate	= pos_port_activate,
	.shutdown	= pos_port_shutdown,
	.carrier_raised = pos_carrier_raised,
	.dtr_rts	= pos_dtr_rts,
};
#endif

static int __init pos_tty_init(void)
{
	int result,i;

	INFO("ttyPos:%s %s\n",DRV_VERSION,VERSION_DATE);

    for(i=0;i<POS_TTY_MINORS;i++)
		pdx_table[i] = NULL;

	pos_tty_driver = tty_alloc_driver(POS_TTY_MINORS, 0);
	if (IS_ERR(pos_tty_driver))
		return -ENOMEM;

	pos_tty_driver->owner = THIS_MODULE;
	pos_tty_driver->driver_name = "usbpos";
	pos_tty_driver->name = 	"ttyPos";
	pos_tty_driver->major = 0;//POS_TTY_MAJOR;//The major number will be chosen dynamically
	pos_tty_driver->minor_start = 0;
	pos_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	pos_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	pos_tty_driver->flags = TTY_DRIVER_REAL_RAW | TTY_USB_DEV;
	pos_tty_driver->init_termios = tty_std_termios;
	pos_tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD
	    | HUPCL | CLOCAL;
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,6,18))
	tty_set_operations(pos_tty_driver, &pos_ops);
#else
	tty_set_operations(pos_tty_driver, (struct tty_operations *)&pos_ops);
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
	for (i = 0; i < POS_TTY_MINORS; i++) 
    {
		tty_port_init(&pos_port[i]);
		pos_port[i].ops = &pos_port_ops;
		pos_port[i].close_delay     = HZ / 2;	/* .5 seconds */
		pos_port[i].closing_wait    = 30 * HZ;/* 30 seconds */
	}
#endif

	result = tty_register_driver(pos_tty_driver);
	if (result) {
		ERR("%s - tty_register_driver failed\n", __func__);
		goto byebye1;
	}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
    for (i = 0; i < POS_TTY_MINORS; i++)
		tty_port_destroy(&pos_port[i]);
#endif

	result = usb_register(&pos_usb_driver);
	if (result) {
		ERR("%s - usb_register failed; err: %d\n",__func__, result);
        goto byebye2;
	}

    
	return 0;
byebye2:
	tty_unregister_driver(pos_tty_driver);
byebye1:
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,15,0))
    tty_driver_kref_put(pos_tty_driver);
#else
    put_tty_driver(pos_tty_driver);
#endif

	return result;
}

static void __exit pos_tty_exit(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))	
    int i;
#endif

	usb_deregister(&pos_usb_driver);
	tty_unregister_driver(pos_tty_driver);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,15,0))
    tty_driver_kref_put(pos_tty_driver);
#else
    put_tty_driver(pos_tty_driver);
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
    for (i = 0; i < POS_TTY_MINORS; i++)
		tty_port_destroy(&pos_port[i]);
#endif

    INFO("pos_tty_exit\n");
}

module_init(pos_tty_init);
module_exit(pos_tty_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
MODULE_ALIAS_LDISC(N_SLIP);
