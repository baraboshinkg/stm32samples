/*
 *                                                                                                  geany_encoding=koi8-r
 * can.c
 *
 * Copyright 2018 Edward V. Emelianov <eddy@sao.ru, edward.emelianoff@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */
#include "can.h"
#include "hardware.h"
#include "proto.h"
#include "usart.h"

#include <string.h> // memcpy

// circular buffer for  received messages
static CAN_message messages[CAN_INMESSAGE_SIZE];
static uint8_t first_free_idx = 0;    // index of first empty cell
static int8_t first_nonfree_idx = -1; // index of first data cell
static uint16_t oldspeed = 100; // speed of last init

static uint16_t CANID = 0xFFFF;
#ifdef EBUG
static uint32_t last_err_code = 0;
#endif
static CAN_status can_status = CAN_STOP;

static void can_process_fifo(uint8_t fifo_num);

CAN_status CAN_get_status(){
    CAN_status st = can_status;
    // give overrun message only once
#ifdef EBUG
    if(st == CAN_FIFO_OVERRUN) MSG("fifo 0 overrun\n");
#endif
    if(st == CAN_FIFO_OVERRUN) can_status = CAN_READY;
    return st;
}

// push next message into buffer; return 1 if buffer overfull
static int CAN_messagebuf_push(CAN_message *msg){
    //MSG("Try to push\n");
    if(first_free_idx == first_nonfree_idx) return 1; // no free space
    if(first_nonfree_idx < 0) first_nonfree_idx = 0;  // first message in empty buffer
    memcpy(&messages[first_free_idx++], msg, sizeof(CAN_message));
    // need to roll?
    if(first_free_idx == CAN_INMESSAGE_SIZE) first_free_idx = 0;
    #ifdef EBUG
    //MSG("1st free: "); printu(first_free_idx); NL();
    #endif
    return 0;
}

// pop message from buffer
CAN_message *CAN_messagebuf_pop(){
    if(first_nonfree_idx < 0) return NULL;
    #ifdef EBUG
    //MSG("read from idx "); printu(first_nonfree_idx); NL();
    #endif
    CAN_message *msg = &messages[first_nonfree_idx++];
    if(first_nonfree_idx == CAN_INMESSAGE_SIZE) first_nonfree_idx = 0;
    if(first_nonfree_idx == first_free_idx){ // buffer is empty - refresh it
        first_nonfree_idx = -1;
        first_free_idx = 0;
#ifdef EBUG
    //  MSG("refresh buffer\n"); NL();
#endif
    }
    return msg;
}

// get CAN address data from GPIO pins
void readCANID(){
    uint8_t CAN_addr = READ_CAN_INV_ADDR();
    CAN_addr = ~CAN_addr & 0x7;
    CANID = (CAN_ID_PREFIX & CAN_ID_MASK) | CAN_addr;
}

uint16_t getCANID(){
    return CANID;
}

void CAN_reinit(uint16_t speed){
    readCANID();
    CAN->TSR |= CAN_TSR_ABRQ0 | CAN_TSR_ABRQ1 | CAN_TSR_ABRQ2;
    RCC->APB1RSTR |= RCC_APB1RSTR_CANRST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_CANRST;
    CAN_setup(speed);
}

/*
Can filtering: FSCx=0 (CAN->FS1R) -> 16-bit identifiers
MASK: FBMx=0 (CAN->FM1R), two filters (n in FR1 and n+1 in FR2)
    ID:   CAN->sFilterRegister[x].FRn[0..15]
    MASK: CAN->sFilterRegister[x].FRn[16..31]
    FR bits:  STID[10:0] RTR IDE EXID[17:15]
LIST: FBMx=1, four filters (n&n+1 in FR1, n+2&n+3 in FR2)
    IDn:   CAN->sFilterRegister[x].FRn[0..15]
    IDn+1: CAN->sFilterRegister[x].FRn[16..31]
*/

/*
Can timing: main freq - APB (PLL=48MHz)
segment = 1sync + TBS1 + TBS2, sample point is between TBS1 and TBS2,
so if TBS1=4 and TBS2=3, sum=8, bit sampling freq is 48/8 = 6MHz
-> to get   100kbps we need prescaler=60
            250kbps - 24
            500kbps - 12
            1MBps   - 6
*/

// speed - in kbps
void CAN_setup(uint16_t speed){
    LED_off(LED1);
    if(speed == 0) speed = oldspeed;
    else if(speed < 50) speed = 50;
    else if(speed > 3000) speed = 3000;
    oldspeed = speed;
    uint32_t tmout = 16000000;
    if(CANID == 0xFFFF) readCANID();
    // Configure GPIO: PB8 - CAN_Rx, PB9 - CAN_Tx
    /* (1) Select AF mode (10) on PB8 and PB9 */
    /* (2) AF4 for CAN signals */
    GPIOB->MODER = (GPIOB->MODER & ~(GPIO_MODER_MODER8 | GPIO_MODER_MODER9))
                 | (GPIO_MODER_MODER8_AF | GPIO_MODER_MODER9_AF); /* (1) */
    GPIOB->AFR[1] = (GPIOB->AFR[1] &~ (GPIO_AFRH_AFRH0 | GPIO_AFRH_AFRH1))\
                  | (4 << (0 * 4)) | (4 << (1 * 4)); /* (2) */
    /* Enable the peripheral clock CAN */
    RCC->APB1ENR |= RCC_APB1ENR_CANEN;
      /* Configure CAN */
    /* (1) Enter CAN init mode to write the configuration */
    /* (2) Wait the init mode entering */
    /* (3) Exit sleep mode */
    /* (4) Normal mode, set timing to 100kb/s: TBS1 = 4, TBS2 = 3, prescaler = 60 */
    /* (5) Leave init mode */
    /* (6) Wait the init mode leaving */
    /* (7) Enter filter init mode, (16-bit + mask, bank 0 for FIFO 0) */
    /* (8) Acivate filter 0 for two IDs */
    /* (9) Identifier list mode */
    /* (10) Set the Id list */
    /* (12) Leave filter init */
    /* (13) Set error interrupts enable */
    CAN->MCR |= CAN_MCR_INRQ; /* (1) */
    while((CAN->MSR & CAN_MSR_INAK)!=CAN_MSR_INAK) /* (2) */
    {
        if(--tmout == 0) break;
    }
    CAN->MCR &=~ CAN_MCR_SLEEP; /* (3) */
    CAN->MCR |= CAN_MCR_ABOM; /* allow automatically bus-off */

    CAN->BTR |=  2 << 20 | 3 << 16 | (6000/speed) << 0; /* (4) */
    CAN->MCR &=~ CAN_MCR_INRQ; /* (5) */
    tmout = 16000000;
    while((CAN->MSR & CAN_MSR_INAK)==CAN_MSR_INAK) if(--tmout == 0) break; /* (6) */
    // accept ALL
    CAN->FMR = CAN_FMR_FINIT; /* (7) */
    CAN->FA1R = CAN_FA1R_FACT0 | CAN_FA1R_FACT1; /* (8) */
    // set to 1 all needed bits of CAN->FFA1R to switch given filters to FIFO1
#if 0
    CAN->FM1R = CAN_FM1R_FBM0; /* (9) */
    CAN->sFilterRegister[0].FR1 = CANID << 5 | ((BCAST_ID << 5) << 16); /* (10) */
#else
    CAN->sFilterRegister[0].FR1 = (1<<21)|(1<<5); // all odd IDs
    CAN->FFA1R = 2; // filter 1 for FIFO1, filter 0 - for FIFO0
    CAN->sFilterRegister[1].FR1 = (1<<21); // all even IDs
#endif
    CAN->FMR &=~ CAN_FMR_FINIT; /* (12) */
    CAN->IER |= CAN_IER_ERRIE | CAN_IER_FOVIE0 | CAN_IER_FOVIE1; /* (13) */

    /* Configure IT */
    /* (14) Set priority for CAN_IRQn */
    /* (15) Enable CAN_IRQn */
    NVIC_SetPriority(CEC_CAN_IRQn, 0); /* (14) */
    NVIC_EnableIRQ(CEC_CAN_IRQn); /* (15) */
    can_status = CAN_READY;
}

void can_proc(){
#ifdef EBUG
    if(last_err_code){
        MSG("Error, ESR=");
        printu(last_err_code);
        NL();
        last_err_code = 0;
    }
#endif
    // check for messages in FIFO0 & FIFO1
    if(CAN->RF0R & CAN_RF0R_FMP0){
        can_process_fifo(0);
    }
    if(CAN->RF1R & CAN_RF1R_FMP1){
        can_process_fifo(1);
    }
    if(CAN->ESR & (CAN_ESR_BOFF | CAN_ESR_EPVF | CAN_ESR_EWGF)){ // much errors - restart CAN BUS
        switchbuff(0);
        SEND("\nToo much errors, restarting!\n");
        SEND("Receive error counter: ");
        printu((CAN->ESR & CAN_ESR_REC)>>24);
        SEND("\nTransmit error counter: ");
        printu((CAN->ESR & CAN_ESR_TEC)>>16);
        SEND("\nLast error code: ");
        int lec = (CAN->ESR & CAN_ESR_LEC) >> 4;
        const char *errmsg = "No";
        switch(lec){
            case 1: errmsg = "Stuff"; break;
            case 2: errmsg = "Form"; break;
            case 3: errmsg = "Ack"; break;
            case 4: errmsg = "Bit recessive"; break;
            case 5: errmsg = "Bit dominant"; break;
            case 6: errmsg = "CRC"; break;
            case 7: errmsg = "(set by software)"; break;
        }
        SEND(errmsg); SEND(" error\n");
        if(CAN->ESR & CAN_ESR_BOFF) SEND("Bus off");
        if(CAN->ESR & CAN_ESR_EPVF) SEND("Passive error limit");
        if(CAN->ESR & CAN_ESR_EWGF) SEND("Error counter limit");
        NL();
        // request abort for all mailboxes
        CAN->TSR |= CAN_TSR_ABRQ0 | CAN_TSR_ABRQ1 | CAN_TSR_ABRQ2;
        // reset CAN bus
        RCC->APB1RSTR |= RCC_APB1RSTR_CANRST;
        RCC->APB1RSTR &= ~RCC_APB1RSTR_CANRST;
        CAN_setup(0);
    }
#if 0
    static uint32_t esr, msr, tsr;
    uint32_t msr_now = CAN->MSR & 0xf;
    if(esr != CAN->ESR || msr != msr_now || tsr != CAN->TSR){
        MSG("Timestamp: ");
        printu(Tms);
        NL();
    }
    if((CAN->ESR) != esr){
        esr = CAN->ESR;
        MSG("CAN->ESR: ");
        printuhex(esr); NL();
    }
    if(msr_now != msr){
        msr = msr_now;
        MSG("CAN->MSR & 0xf: ");
        printuhex(msr); NL();
    }
    if(CAN->TSR != tsr){
        tsr = CAN->TSR;
        MSG("CAN->TSR: ");
        printuhex(tsr); NL();
    }
#endif
}

CAN_status can_send(uint8_t *msg, uint8_t len, uint16_t target_id){
    uint8_t mailbox = 0;
    // check first free mailbox
    if(CAN->TSR & (CAN_TSR_TME)){
        mailbox = (CAN->TSR & CAN_TSR_CODE) >> 24;
    }else{ // no free mailboxes
#ifdef EBUG
        MSG("No free mailboxes"); NL();
#endif
        return CAN_BUSY;
    }
#ifdef EBUG
    MSG("Send data. Len="); printu(len);
    SEND(", tagid="); printuhex(target_id);
    SEND(", data=");
    for(int i = 0; i < len; ++i){
        SEND(" "); printuhex(msg[i]);
    }
    NL();
#endif
    CAN_TxMailBox_TypeDef *box = &CAN->sTxMailBox[mailbox];
    uint32_t lb = 0, hb = 0;
    switch(len){
        case 8:
            hb |= (uint32_t)msg[7] << 24;
            __attribute__((fallthrough));
        case 7:
            hb |= (uint32_t)msg[6] << 16;
            __attribute__((fallthrough));
        case 6:
            hb |= (uint32_t)msg[5] << 8;
            __attribute__((fallthrough));
        case 5:
            hb |= (uint32_t)msg[4];
            __attribute__((fallthrough));
        case 4:
            lb |= (uint32_t)msg[3] << 24;
            __attribute__((fallthrough));
        case 3:
            lb |= (uint32_t)msg[2] << 16;
            __attribute__((fallthrough));
        case 2:
            lb |= (uint32_t)msg[1] << 8;
            __attribute__((fallthrough));
        default:
            lb |= (uint32_t)msg[0];
    }
    box->TDLR = lb;
    box->TDHR = hb;
    box->TDTR = len;
    box->TIR  = (target_id & 0x7FF) << 21 | CAN_TI0R_TXRQ;
    return CAN_OK;
}

void can_send_dummy(){
    uint8_t msg = CMD_TOGGLE;
    if(CAN_OK != can_send(&msg, 1, TARG_ID)) SEND("Bus busy!\n");
    MSG("CAN->MSR: ");
    printuhex(CAN->MSR); NL();
    MSG("CAN->TSR: ");
    printuhex(CAN->TSR); NL();
    MSG("CAN->ESR: ");
    printuhex(CAN->ESR); NL();
}

void can_send_broadcast(){
    uint8_t msg = CMD_BCAST;
    if(CAN_OK != can_send(&msg, 1, BCAST_ID)) SEND("Bus busy!\n");
    MSG("Broadcast message sent\n");
}

static void can_process_fifo(uint8_t fifo_num){
    if(fifo_num > 1) return;
    LED_on(LED1); // Turn on LED1 - message received
    CAN_FIFOMailBox_TypeDef *box = &CAN->sFIFOMailBox[fifo_num];
    volatile uint32_t *RFxR = (fifo_num) ? &CAN->RF1R : &CAN->RF0R;
    /*
    MSG("\nReceive, RDTR=");
    #ifdef EBUG
    printuhex(box->RDTR);
    NL();
    #endif
    */
    // read all
    while(*RFxR & CAN_RF0R_FMP0){ // amount of messages pending
        // CAN_RDTxR: (16-31) - timestamp, (8-15) - filter match index, (0-3) - data length
        /* TODO: check filter match index if more than one ID can receive */
        CAN_message msg;
        uint8_t *dat = msg.data;
        uint8_t len = box->RDTR & 0x7;
        msg.length = len;
        msg.ID = box->RIR >> 21;
        msg.filterNo = (box->RDTR >> 8) & 0xff;
        msg.fifoNum = fifo_num;
        if(len){ // message can be without data
            uint32_t hb = box->RDHR, lb = box->RDLR;
            switch(len){
                case 8:
                    dat[7] = hb>>24;
                    __attribute__((fallthrough));
                case 7:
                    dat[6] = (hb>>16) & 0xff;
                    __attribute__((fallthrough));
                case 6:
                    dat[5] = (hb>>8) & 0xff;
                    __attribute__((fallthrough));
                case 5:
                    dat[4] = hb & 0xff;
                    __attribute__((fallthrough));
                case 4:
                    dat[3] = lb>>24;
                    __attribute__((fallthrough));
                case 3:
                    dat[2] = (lb>>16) & 0xff;
                    __attribute__((fallthrough));
                case 2:
                    dat[1] = (lb>>8) & 0xff;
                    __attribute__((fallthrough));
                case 1:
                    dat[0] = lb & 0xff;
            }
        }
        if(CAN_messagebuf_push(&msg)) return; // error: buffer is full, try later
        *RFxR |= CAN_RF0R_RFOM0; // release fifo for access to next message
    }
    //if(*RFxR & CAN_RF0R_FULL0) *RFxR &= ~CAN_RF0R_FULL0;
    *RFxR = 0; // clear FOVR & FULL
}

void cec_can_isr(){
    if(CAN->RF0R & CAN_RF0R_FOVR0){ // FIFO overrun
        CAN->RF0R &= ~CAN_RF0R_FOVR0;
        can_status = CAN_FIFO_OVERRUN;
    }
    if(CAN->RF1R & CAN_RF1R_FOVR1){
        CAN->RF1R &= ~CAN_RF1R_FOVR1;
        can_status = CAN_FIFO_OVERRUN;
    }
    if(CAN->MSR & CAN_MSR_ERRI){ // Error
        CAN->MSR &= ~CAN_MSR_ERRI;
        // request abort for problem mailbox
        if(CAN->TSR & CAN_TSR_TERR0) CAN->TSR |= CAN_TSR_ABRQ0;
        if(CAN->TSR & CAN_TSR_TERR1) CAN->TSR |= CAN_TSR_ABRQ1;
        if(CAN->TSR & CAN_TSR_TERR2) CAN->TSR |= CAN_TSR_ABRQ2;
#ifdef EBUG
        last_err_code = CAN->ESR;
#endif
    }
}
