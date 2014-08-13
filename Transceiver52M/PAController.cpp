/* 
 * Written by Kurtis Heimerl <kheimerl@cs.berkeley.edu>
 *
 * Copyright (c) 2011 Kurtis Heimerl
 *
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * Neither the name of the project's author nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdio.h>
#include <time.h>
#include <Logger.h>
#include <fcntl.h>
#include <string.h>
#include <Configuration.h>
#include <ftdi.h>
#include "PAController.h"
#include "config.h"

#if defined USE_UHD || defined USE_USRP1
#define DONT_USE_SERIAL 1
#endif

extern ConfigurationTable gConfig;

using namespace std;

//I hate C++ -kurtis

#define DEFAULT_START_TIME "00:00"
#define DEFAULT_END_TIME "00:00"
#define TIME_FORMAT "%H:%M"

#define FTDI_VENDOR_ID   0x0403
#define FTDI_PRODUCT_ID  0x6001

static bool pa_on = false;
static Mutex pa_lock;
static time_t last_update = NULL;

static struct tm start_tm;
static struct tm end_tm;

#ifndef DONT_USE_SERIAL
static int fd1;
static string on_cmd;
static string off_cmd;

/* For the FTDI chipset. */
static struct ftdi_context *ftdi = NULL;
static unsigned char ftdi_buf[1];
#endif

//hack for now, as I want one source file for both RAD1 and UHD/USRP1

/* assumes you hold the lock */
static void actual_pa_off(string reason){
    LOG(ALERT) << "PA Off:" << pa_on << ":" << reason;
    pa_on = false;
#ifndef DONT_USE_SERIAL

    if ( ftdi ) {
      ftdi_buf[0] = 0; /* The first relay switch. */
      ftdi_write_data(ftdi, ftdi_buf, 1);
    } else {
      fcntl(fd1,F_SETFL,0);
      write(fd1,off_cmd.c_str(), off_cmd.length());
      write(fd1,off_cmd.c_str(), off_cmd.length());
    }
#endif
}

static void turn_pa_on(bool resetTime, string reason){
    ScopedLock lock (pa_lock);
    //don't think I need to garbage collect, it's just an int
    if (!pa_on || resetTime){
	LOG(ALERT) << "PA On:" << pa_on << ":" << reason;
	last_update = time(NULL);
	pa_on = true;
#ifndef DONT_USE_SERIAL
	if ( ftdi ) {
	  ftdi_buf[0] = 1; /* The first relay switch. */
	  ftdi_write_data(ftdi, ftdi_buf, 1);
	} else {
	  fcntl(fd1,F_SETFL,0);
	  write(fd1,on_cmd.c_str(), on_cmd.length());
	  write(fd1,on_cmd.c_str(), on_cmd.length());
	}
#endif
    }
}

static void turn_pa_off(string reason){
  ScopedLock lock (pa_lock);
  actual_pa_off(reason);
}

/* key point: this is being called all the time
   by the transceiver, allowing it to be updated
   almost immediately after time stamp ends */
bool update_pa(){

    //first check for time
    time_t rawtime;
    struct tm * timeinfo; //this is statically defined in library, no need to free
    time(&rawtime);
    timeinfo = localtime(&rawtime);

    //exit if we're after start time and before end time
    if (((timeinfo->tm_hour > start_tm.tm_hour) ||
	 (timeinfo->tm_hour == start_tm.tm_hour && timeinfo->tm_min > start_tm.tm_min)) &&
	((timeinfo->tm_hour < end_tm.tm_hour) || 
	 (timeinfo->tm_hour == end_tm.tm_hour &&  timeinfo->tm_min < end_tm.tm_min))){
	turn_pa_on(false, "Time of Day");
	return pa_on;
    }

    //otherwise see if we should turn the PA off
    int pa_timeout = gConfig.getNum("VBTS.PA.Timeout");
    ScopedLock lock (pa_lock);
    if (pa_on && last_update && 
	rawtime > pa_timeout + last_update){
	actual_pa_off("Timeout");
	LOG(ALERT) << "Timeout:" << pa_timeout;
    }
    return pa_on;
}

//the "turn the PA on method"
class on_method : public xmlrpc_c::method {
public:
    on_method() {
	this->_signature = "n:";
	this->_help = "This method turns the PA on";
    }
    void
    execute(xmlrpc_c::paramList const& paramList,
	    xmlrpc_c::value *   const  retvalP) {
	turn_pa_on(true, "None");
	*retvalP = xmlrpc_c::value_nil();
    }
};

//the "turn the PA on method"
class on_method_reason : public xmlrpc_c::method {
public:
    on_method_reason() {
	this->_signature = "n:s";
	this->_help = "This method turns the PA on and records the reason";
    }
    void
    execute(xmlrpc_c::paramList const& paramList,
	    xmlrpc_c::value *   const  retvalP) {
	turn_pa_on(true, paramList.getString(0));
	*retvalP = xmlrpc_c::value_nil();
    }
};

//the "turn the PA on method"
class off_method : public xmlrpc_c::method {
public:
    off_method() {
	this->_signature = "n:"; 
	this->_help = "This method turns the PA off";
    }
    void
    execute(xmlrpc_c::paramList const& paramList,
	    xmlrpc_c::value *   const  retvalP) {
	turn_pa_off("None");
	*retvalP = xmlrpc_c::value_nil();
    }
};

//the "turn the PA on method"
class off_method_reason : public xmlrpc_c::method {
public:
    off_method_reason() {
	this->_signature = "n:s"; 
	this->_help = "This method turns the PA off and records the reason";
    }
    void
    execute(xmlrpc_c::paramList const& paramList,
	    xmlrpc_c::value *   const  retvalP) {
	turn_pa_off(paramList.getString(0));
	*retvalP = xmlrpc_c::value_nil();
    }
};

class status_method : public xmlrpc_c::method {
public:
    status_method() {
	this->_signature = "b:";
	this->_help = "This method returns the PA status";
    }
    void
    execute(xmlrpc_c::paramList const& paramList,
	    xmlrpc_c::value *   const  retvalP) {
	*retvalP = xmlrpc_c::value_boolean(update_pa());
    }
};

/* please instantiate me only once -kurtis*/
PAController::PAController()
{
    
    registry = new xmlrpc_c::registry();
    
    xmlrpc_c::methodPtr const onMethod(new on_method);
    xmlrpc_c::methodPtr const onMethodReason(new on_method_reason);
    xmlrpc_c::methodPtr const offMethod(new off_method);
    xmlrpc_c::methodPtr const offMethodReason(new off_method_reason);
    xmlrpc_c::methodPtr const statusMethod(new status_method);
    
    registry->addMethod("on", onMethod);
    registry->addMethod("onWithReason", onMethodReason);
    registry->addMethod("off", offMethod);
    registry->addMethod("offWithReason", offMethodReason);
    registry->addMethod("status", statusMethod);
    
    long rpc_port = gConfig.getNum("VBTS.PA.RPCPort");
    string rpc_log = gConfig.getStr("VBTS.PA.RPCLogLoc");
    
    RPCServer = new xmlrpc_c::serverAbyss(*registry,
					  rpc_port,
					  rpc_log
	);

#ifndef DONT_USE_SERIAL
    string serial_loc = gConfig.getStr("VBTS.PA.SerialLoc");
    fd1 = open (serial_loc.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);

    on_cmd = gConfig.getStr("VBTS.PA.OnCommand");
    //on_cmd = "O0=1\r";
    off_cmd = gConfig.getStr("VBTS.PA.OffCommand");
    //off_cmd = "O0=0\r";

    /* Enable FTDI chip, if present and working. */
    enableFTDIChip();
#endif

    string start_time = gConfig.getStr("VBTS.PA.StartTime");
    string end_time = gConfig.getStr("VBTS.PA.EndTime");

    if (strptime(start_time.c_str(), TIME_FORMAT, &start_tm) ==  NULL){
	LOG(ALERT) << "MALFORMED START TIME";
	strptime(DEFAULT_START_TIME, TIME_FORMAT, &start_tm);
    }
    
    if (strptime(end_time.c_str(), TIME_FORMAT, &end_tm) ==  NULL){
	LOG(ALERT) << "MALFORMED END TIME";
	strptime(DEFAULT_END_TIME, TIME_FORMAT, &end_tm);
    }

}

#ifndef DONT_USE_SERIAL
/*
 * Following is for the new board, using FTDI IC in
 * bitbang mode. It calls on the FTDI using the libftdi via
 * libusb.
 *
 * If successful, the ftdi_context will be allocated, and the
 * FTDI chipset operation mode will be set to 'bitbang'.
 *
 */

void
PAController::enableFTDIChip() 
{
  
  int ret = -1; /* -ve, to be consistent with the libftdi error code. */

  /* Allocate and initialize a new ftdi_context.
   * Memory is allocated for the 'ftdi'.
   */
  ftdi = ftdi_new();

  if ( ftdi == NULL ){
    LOG(ALERT) << "ftdi_new failed!";
    return;
  }
  
  /* Open the FTDI device on the USB interface.
   */
  ret = ftdi_usb_open(ftdi, FTDI_VENDOR_ID, FTDI_PRODUCT_ID);
  
  if( ret < 0 && ret != -5 ) {

    /* Error code -5 is return when the program is unable to claim
     * the USB device. We can ignore that for now.
     * See FTDI implementation for details.
     */
    
    /* If no device is found means two things:
     * 1.) the FTDI-based rely controller isn't being used.
     * 2.) FTDI is used but is not operational.
     *
     * As we can't differentiate these two scenario programatically,
     * without user input, treating them same. 
     * No error is logged.
     *
     * TODO: add this into OpenBTS configuration.
     */
    goto free_ret;
  } else {
    
    /* Enable bitbang mode. The bitmask is set to match the 
     * first relay switch. Setting the bitmask to 0xFF will 
     * enable all the pins to be IO. 
     */
    ret = ftdi_set_bitmode(ftdi, 0xFF, BITMODE_BITBANG);
    
    if ( ret < 0 ) {
      goto usb_close;
    }
  }
  
  /* Test the IO pins. If IO test pins fails, we don't activate the
   * relay switch. Report error, cleanup, and turn to fail safe mode.
   */
  
  if ( !testFTDIIOPins() ) {
    LOG(ALERT) << "FTDI IO Pin test fail, disabling the FTDI relay switch controller";
    goto usb_close;
  } 

  return;
  
 usb_close:
  ftdi_usb_close(ftdi);
 free_ret:
  ftdi_free(ftdi);
  ftdi=NULL;
}

/* Test the IO pins in bitbang mode. 
 * Turn off/on/off all the relay switches, with 3 seconds delay
 * in between. This is also important to ensure we are in steady
 * programatic state for the IO/relay.
 */

int
PAController::testFTDIIOPins(void) 
{

  int ret;

  if ( ftdi == NULL ) 
    return FALSE;

  ftdi_buf[0] = 0x0; /* Turn everything off. */
  ret = ftdi_write_data(ftdi, ftdi_buf, 1);
  if (ret < 0) {
    LOG(ALERT) << "FTDI OFF Write failed: error(%d): %s \n", 
      ftdi_buf[0], ret, ftdi_get_error_string(ftdi);
    return FALSE;
  }

  usleep(250000); /* Wait 250ms. */
  ftdi_buf[0] = 0xFF; /*Turn everything on. */
  ret = ftdi_write_data(ftdi, ftdi_buf, 1);
  if (ret < 0) {
    LOG(ALERT) << "FTDI ON Write failed: error(%d): %s \n", 
      ftdi_buf[0], ret, ftdi_get_error_string(ftdi);
    return FALSE;
  }
  
  usleep(250000);
  ftdi_buf[0] = 0x0; /* Turn everything off. */
  ret = ftdi_write_data(ftdi, ftdi_buf, 1);
  if (ret < 0) {
    LOG(ALERT) << "FTDI OFF Write failed: error(%d): %s \n", 
      ftdi_buf[0], ret, ftdi_get_error_string(ftdi);
    return FALSE;
  }

  return TRUE;
}
#endif

PAController::~PAController()
{
    //should call the deconstructor and close cleanly... right?
    delete RPCServer;
    delete registry;

#ifndef DONT_USE_SERIAL
    /* 
     * Change the relay to steady state (close), disable FTDI 
     * bitbang mode, clear ftdi_context memory and close USB.
     */
    if ( ftdi ) {
      ftdi_buf[0] = 0x0; // Turn off.
      ftdi_write_data(ftdi, ftdi_buf, 1);
      ftdi_disable_bitbang(ftdi);
      ftdi_usb_close(ftdi);
      ftdi_free(ftdi);
      ftdi=NULL;
    }
#endif
}

void PAController::run()
{
    RPCServer->run();
}

void PAController::on(string reason)
{
    turn_pa_on(false, reason);
}

void PAController::off(string reason)
{
    turn_pa_off(reason);
}

bool PAController::state()
{
    return update_pa();
}

/* non-member functions */
void runController(PAController* cont)
{
    Thread RPCThread;
    RPCThread.start((void*(*)(void*)) &PAController::run, cont);
    cont->on("Starting");
}
