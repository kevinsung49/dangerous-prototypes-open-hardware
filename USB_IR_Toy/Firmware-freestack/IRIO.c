/*
 *
 *	USB infrared remote control receiver transmitter firmware v1.0
 *	License: creative commons - attribution, share-alike
 *	Copyright Ian Lesnet 2010
 *	http://dangerousprototypes.com
 *
 */

#include "globals.h"
#include "PICUSB.h"


void UsbDeviceTasks(void);
void irIOSend8Bit(void);

//extern struct _irtoy irToy;
extern unsigned char cdc_trf_state;
extern BYTE cdc_In_buffer[64];
extern BYTE cdc_Out_buffer[64];
extern BYTE cdc_Out_bufferA[64];
extern BYTE cdc_Out_bufferB[64];
extern BYTE cdc_In_bufferA[64];
extern BYTE cdc_In_bufferB[64];
extern BYTE *InPtr;
extern BYTE *OutPtr;

static struct {
    unsigned char T1offsetH;
    unsigned char T1offsetL;
    unsigned char RXsamples;
    unsigned char TXsamples;
    unsigned char timeout;
    unsigned char btrack; //bit tracker
    unsigned char btrackreset; //reset value for bit tracker
    unsigned char work;
    unsigned char rxbuf;
    unsigned char txbuf;
    unsigned char TX : 1;
    unsigned char rxflag : 1;
    unsigned char txflag : 1;
    unsigned char flushflag : 1;
    unsigned char sendzzz : 1;
} irIO;

static enum _smio { //in out data state machine
    I_IDLE = 0,
    I_PARAMETERS,
    I_PROCESS,
    I_LASTPACKET,
} irIOstate = I_IDLE;

void irIOsetup(void) {
    BYTE dummy;
    T2IE = 0; //disable any Timer 2 interrupt
    IRRXIE = 0; //enable RX interrupts for data ACQ
    dummy = IRRX_PORT; // Init read for PORTB IOR
    //send version string
    //   if (WaitInReady()) { //it's always ready, but this could be done better
    WaitInReady();
    cdc_In_buffer[0] = 'X'; //answer OK
    cdc_In_buffer[1] = '0';
    cdc_In_buffer[2] = '1';
    putUnsignedCharArrayUsbUsart(cdc_In_buffer, 3);
    //   }

    //setup for IR TX
    /*
     * PWM registers configuration
     * Fosc = 48000000 Hz
     * Fpwm = 36144.58 Hz (Requested : 36000 Hz)
     * Duty Cycle = 50 %
     * Resolution is 10 bits
     * Prescaler is 4
     * Ensure that your PWM pin is configured as digital output
     * see more details on http://www.micro-examples.com/
     * this source code is provided 'as is',
     * use it at your own risks
     * http://www.micro-examples.com/public/microex-navig/doc/097-pwm-calculator
     */
    IRTX_TRIS |= IRTX_PIN; //digital INPUT (no PWM until active)
    IRTX_LAT &= (~IRTX_PIN);
    T2IF = 0; //clear the interrupt flag
    T2IE = 0; //disable interrupts
    PR2 = 0b01010010; //82
    T2CON = 0b00000101;
    CCPR1L = 0b00101001; //upper 8 bits of duty cycte
    CCP1CON = 0b00011100; //should be cleared on exit! (5-4 two LSB of duty, 3-0 set PWM)

    //setup for IR RX
    irIO.btrack = 0;
    irIO.rxflag = 0;
    irIO.txflag = 0;
    irIO.flushflag = 0;
    irIO.timeout = 0;
    irIO.RXsamples = 0;
    irIO.TXsamples = 0;
    irIO.TX = 0;
    irIO.sendzzz = 0;
    irIO.T1offsetH = T1_OFFSETH;
    irIO.T1offsetL = T1_OFFSETL;
    irIO.btrackreset = 0b01000000; //preload with 7bit value

    IRRXIE = 1; //IR RX interrupt on

}
//
// irIO periodic service routine
// moves bytes in and out

unsigned char irIOservice(void) {
    static unsigned char c;

    static struct _smCommand {
        unsigned char command[5];
        unsigned char parameters;
        unsigned char parCnt;
    } irIOcommand;

    //TO DO:
    //variable names, centralize variables
    //playback last recording
    //PWM, timer setup
    //0xxxxxxx - command
    //00000000 - reset, return to RC5 (same as SUMP) LED output/off, CPP1CON=0; T2ON=0; T1ON=0; T1IE=0;
    //1xxxxxxx - data

    if (irIO.TXsamples == 0) {
        irIO.TXsamples = getUnsignedCharArrayUsbUart(irToy.s, CDC_BUFFER_SIZE); //JTR2
        c = 0;
    }
    if (irIO.TXsamples > 0) {
        switch (irIOstate) {
            case I_IDLE:

                if ((irToy.s[c] & 0b10000000) != 0) {//7bit + data packet bit
                    if (irIO.TX == 0) {//not transmitting, disable RX, setup TX
                        LedOn(); //LED on
                        irIO.TX = 1;
                        IRRXIE = 0; //disable RX interrupt
                        T1CON = 0;

                        irIO.flushflag = 1; //force flush of remaining bytes

                        irIO.txbuf = irToy.s[c];

                        irIO.txflag = 1; //set tx flag
                        irIO.TXsamples--;
                        c++;

                        //T1_10kHzOffset();
                        TMR1H = irIO.T1offsetH;
                        TMR1L = irIO.T1offsetL;
                        T1IF = 0; //clear the interrupt flag
                        T1IE = 1; //able interrupts...
                        T1ON = 1; //timer on

                    } else if (irIO.txflag == 0) { //already on, but need to load new byte
                        irIO.txbuf = irToy.s[c];
                        c++;
                        irIO.txflag = 1; //set tx flag
                        irIO.TXsamples--;
                    } //both are false, we just try again on the next loop
                } else { //process as command
#define IRIO_RESET 0x00
#define IRIO_SETUP_SAMPLETIMER 0x01
#define IRIO_SETUP_PWM 0x02
#define IRIO_RAW 0x03
#define IRIO_REPLAY 0x04
#define IRIO_SENDZZZ 0x05 // JTR3

                    switch (irToy.s[c]) {
                        case IRIO_RESET: //reset, return to RC5 (same as SUMP)
                            CCP1CON = 0;
                            T2ON = 0;
                            T1ON = 0;
                            T1IE = 0;
                            IRRXIE = 0;
                            IRTX_TRIS |= IRTX_PIN; //digital INPUT (no PWM until active)
                            IRTX_LAT &= (~IRTX_PIN); //direction 0
                            LedOff();
                            LedOut();
                            return 1; //need to flag exit!
                            break;
                        case IRIO_SETUP_SAMPLETIMER: //setup the values for the 10khz default sampleing timer
                        case IRIO_SETUP_PWM: //setup PWM frequency
                            irIOcommand.command[0] = irToy.s[c];
                            irIOcommand.parameters = 2;
                            irIOstate = I_PARAMETERS;
                            break;

                        case IRIO_RAW:
                            irIOSend8Bit();
                            break;
                            //case IRIO_REPLAY: //not implemented yet
                            //	break;
                        case IRIO_SENDZZZ:
                            irIO.sendzzz = 1;
                            break;
                        default:
                            break;
                    }
                    irIO.TXsamples--;
                    c++;
                }
                break;
            case I_PARAMETERS://get optional parameters
                irIOcommand.command[irIOcommand.parCnt] = irToy.s[c]; //store each parameter
                irIO.TXsamples--;
                c++;
                irIOcommand.parCnt++;
                if (irIOcommand.parCnt < irIOcommand.parameters) break; //if not all parameters, quit
            case I_PROCESS: //process long commands
                switch (irIOcommand.command[0]) {
                    case IRIO_SETUP_SAMPLETIMER: //set user values for the 10khz default sampling timer
                        irIO.T1offsetH = irIOcommand.command[1];
                        irIO.T1offsetL = irIOcommand.command[2];
                        break;
                    case IRIO_SETUP_PWM: //setup user defined PWM frequency
                        T2CON = 0;
                        PR2 = irIOcommand.command[1]; //user period
                        CCPR1L = (irIOcommand.command[1] >> 1); //upper 8 bits of duty cycle, 50% of period by binary division
                        if ((irIOcommand.command[1]& 0b1) != 0)//if LSB is set, set bit 1 in CCP1CON
                            CCP1CON = 0b00101100; //5-4 two LSB of duty, 3-0 set PWM
                        else
                            CCP1CON = 0b00001100; //5-4 two LSB of duty, 3-0 set PWM

                        T2CON = 0b00000101; //enable timer again, 4x prescaler
                        break;
                }
                irIOstate = I_IDLE; //return to idle state
                break;
        }//switch
    }
    return 0; //CONTINUE
}

void irIOSend8Bit(void) {
    BYTE i;

    irIO.btrackreset = 0b10000000; //load 8 bit bit tracker into variable
    ArmCDCOutDB();
    LedOff();
    IRRXIE = 0; //disable RX interrupt
    T1CON = 0;
    T0_IE = 0;
    T1IE = 0;
    T1IF = 0; //clear the interrupt flag
    //irIO.flushflag = 1; //force flush of remaining bytes
    irIO.TX = 0;
    irIO.txflag = 0; //transmit flag =0 reset the transmit flag

    do {
        irIO.TXsamples = getCDC_Out_ArmNext();

        if (((*OutPtr == 0x5A) && (*(OutPtr + 1) = 0x5A) && (*(OutPtr + 2) = 0x5A)) && (irIO.sendzzz)) {
            irIOstate = I_LASTPACKET;
        } else {
            for (i = 0; i < irIO.TXsamples; i++, OutPtr++) {

                while (irIO.txflag == 1) {
                    FAST_usb_handler();
                }
                irIO.txbuf = *(OutPtr);
                irIO.txflag = 1; //reset the interrupt buffer full flag

                if (irIO.TX == 0) {//enable interrupt if this is the first time
                    irIO.TX = 1;
                    if (!TestUsbInterruptEnabled()) {
                        FAST_usb_handler();
                    }
                    TMR1H = irIO.T1offsetH;
                    TMR1L = irIO.T1offsetL;
                    T1IF = 0; //clear the interrupt flag
                    T1IE = 1; //able interrupts...
                    T1ON = 1; //timer on
                    LedOn();
                }
            }//for
        }
    } while (irIOstate != I_LASTPACKET); //(irIOstate != I_LAST_PACKET);
    DisArmCDCOutDB();
}
//high priority interrupt routine
//#pragma interrupt irIOInterruptHandlerHigh

void irIOInterruptHandlerHigh(void) {
    //irIO driver
    if (IRRXIE == 1 && IRRXIF == 1) { //if RB Port Change Interrupt

        if (((IRRX_PORT & IRRX_PIN) == 0)) {//only if 0, must read PORTB to clear RBIF
            IRRXIE = 0; //disable port b interrupt
            LedOn(); //LED ON

            //TIMER sample rate,
            T1CON = 0;
            //T1_10kHzOffset();
            TMR1H = irIO.T1offsetH;
            TMR1L = irIO.T1offsetL;
            T1IF = 0; //clear the interrupt flag
            T1IE = 1; //able interrupts...
            T1ON = 1; //timer on
            ArmCDCInDB();
            //setup the sample storage buffer
            *InPtr = 0b01000000; //preload a pretty value
            irIO.btrack = 0b00100000;
            irIO.rxflag = 0;
            irIO.timeout = 0; //reset timeout
        }

        IRRXIF = 0; //Reset the RB Port Change Interrupt Flag bit

    } else if (T1IE == 1 && T1IF == 1) { //is this timer 1 interrupt?
        T1ON = 0; //timer off
        //T1_10kHzOffset();//preload timer values
        TMR1H = irIO.T1offsetH;
        TMR1L = irIO.T1offsetL;
        T1ON = 1; //timer back on

        if (irIO.TX == 1) {
            if (irIO.btrack == 0 && irIO.txflag == 1) {
                irIO.work = *OutPtr; //get next byte
                irIO.txflag = 0; //buffer ready for new byte
                irIO.btrack = irIO.btrackreset;
            }

            if (irIO.btrack != 0) {
                if ((*InPtr & irIO.btrack) == 0) {
                    TRISCbits.TRISC2 = 1; //~(irIO.work & irIO.btrack);//put 1 or 0 on the pin
                } else {
                    TRISCbits.TRISC2 = 0; //~(irIO.work & irIO.btrack);//put 1 or 0 on the pin
                }
                irIO.btrack >>= 1; //shift one bit
            } else {//no bits left //shut down PWM, resume RX
                IRTX_TRIS |= IRTX_PIN; //digital INPUT (PWM off)
                LedOff(); //LED off
                irIO.TX = 0; //go back to RX mode
                T1IE = 0; //disable timer2 interrupt
                T1ON = 0;
                IRRXIE = 1; //start portB interrupt again to get new data next time
            }
        } else {

            if ((IRRX_PORT & IRRX_PIN) == 0) //inverse current pin reading
                *InPtr |= irIO.btrack;

            irIO.btrack >>= 1; //shift one bit
            if (irIO.btrack == 0) {//no bits left

                if (*InPtr != 0) { //track how many samples since last IR signal
                    irIO.timeout = 0; //reset timeout
                } else {
                    irIO.timeout++;
                    if (irIO.timeout == 5) {//50 bytes without change (make define)
                        T1ON = 0; //disable timer
                        LedOff(); //LED off
                        irIO.flushflag = 1; //flush any remaining bytes in the sampling buffer
                    }
                }
                irIO.RXsamples++;
                InPtr++;

                if (((irIO.RXsamples == CDC_BUFFER_SIZE - 2) || (irIO.flushflag == 1))) { //if we have full buffer, or end of capture flush

                    SendCDC_In_ArmNext(irIO.RXsamples);
                    /*
                                        WaitInReady();

                                        if (IsInBufferA) {
                                            Inbdp->BDADDR = cdc_In_bufferA;
                                            InPtr = cdc_In_bufferB;
                                        } else {
                                            Inbdp->BDADDR = cdc_In_bufferB;
                                            InPtr = cdc_In_bufferA;
                                        }
                                        Inbdp->BDCNT = irIO.RXsamples;
                                        Inbdp->BDSTAT = ((Inbdp->BDSTAT ^ DTS) & DTS) | UOWN | DTSEN;
                                        IsInBufferA ^= 0xFF;
                     */

                    irIO.RXsamples = 0;

                    if (irIO.flushflag == 1) {
                        irIOstate = I_IDLE;
                        DisArmCDCInDB();
                        if (irIO.sendzzz) {

                            cdc_In_buffer[0] = 'z';
                            cdc_In_buffer[1] = 'z';
                            cdc_In_buffer[2] = 'z';
                            putUnsignedCharArrayUsbUsart(cdc_In_buffer, 3); //send current buffer to USB
                        }

                        IRRXIE = 1; //start portB interrupt again to get new data next time
                    } // End if [timeout flushing]
                } // End if [buffer send conditions.]
                *InPtr = 0;
                irIO.btrack = 0b10000000;
            } // End if [byte complete]
        } // End else [RX mode]
        T1IF = 0;
    }
}