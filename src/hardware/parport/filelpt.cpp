/*
 *  Copyright (C) 2002-2020  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include "dosbox.h"

#include "parport.h"
#include "filelpt.h"
#include "callback.h"
#include "pic.h"
#include "hardware.h" //OpenCaptureFile
#include <stdio.h>

#include "printer_charmaps.h"
#if defined(WIN32)
#include "Shellapi.h"
#endif

CFileLPT::CFileLPT (Bitu nr, uint8_t initIrq, CommandLine* cmd)
                              :CParallel (cmd, nr,initIrq) {
    bool is_file = false;
	InstallationSuccessful = false;
	fileOpen = false;
	controlreg = 0;
    timeout = ~0u;
	std::string str;
	ack = false;

	// add a formfeed when closing?
	if(cmd->FindStringBegin("addFF",str,false))	addFF = true;
	else addFF = false;

	// add a formfeed when closing?
	if(cmd->FindStringBegin("addLF",str,false))	addLF = true;
	else addLF = false;

	// find the codepage
	unsigned int temp=0;
	codepage_ptr = NULL;
	if(cmd->FindStringBegin("cp:",str,false)) {
		if(sscanf(str.c_str(), "%u",&temp)!=1) {
			LOG_MSG("parallel%d: Invalid codepage parameter.",(int)nr+1);
			return;
		} else {
			Bitu i = 0;
			while(charmap[i].codepage!=0) {
				if(charmap[i].codepage==temp) {
					codepage_ptr = charmap[i].map;
					break;
				}
				i++;
			}
		}
	}
	temp=0;

	if(cmd->FindStringBegin("dev:",str,false)) {
		name = str.c_str();
		filetype = FILE_DEV;
	} else if(cmd->FindStringBegin("file:",str,false)) {
		name = str.c_str();
		filetype = FILE_DEV;
        is_file = true;
	} else if(cmd->FindStringBegin("append:",str,false)) {
		name = str.c_str();
		filetype = FILE_APPEND;
	} else filetype = FILE_CAPTURE;

	if (cmd->FindStringFullBegin("openps:",str,false)) {
		action1 = str.c_str();
    }
	if (cmd->FindStringFullBegin("openpcl:",str,false)) {
		action2 = str.c_str();
    }
	if (cmd->FindStringFullBegin("openwith:",str,false)) {
		action3 = str.c_str();
    }
	if (cmd->FindStringFullBegin("openerror:",str,false)) {
		action4 = str.c_str();
    }

	if (cmd->FindStringBegin("timeout:",str,false)) {
		if(sscanf(str.c_str(), "%u",&timeout)!=1) {
			LOG_MSG("parallel%d: Invalid timeout parameter.",(int)nr+1);
			return;
		}
	}

	if (timeout == ~0u)
		timeout = is_file ? 0 : 500;

	InstallationSuccessful = true;
}

CFileLPT::~CFileLPT () {
	// close file
	if(fileOpen)
		fclose(file);
	// remove tick handler
	removeEvent(0);
}

char bufput[105];
int bufct = 0;
static char sig1PCL[] = "\x1b%-12345X@", sig2PCL[] = "\x1b\x45", sigPS[] = "\n%!PS";
bool CFileLPT::OpenFile() {
	switch(filetype) {
	case FILE_DEV:
		file = fopen(name.c_str(),"wb");
        if (file != NULL) setbuf(file,NULL); // disable buffering
		break;
	case FILE_CAPTURE:
		file = OpenCaptureFile("Parallel Port Stream",".prt");
        if (file != NULL) setbuf(file,NULL); // disable buffering
		break;
	case FILE_APPEND:
		file = fopen(name.c_str(),"ab");
        if (file != NULL) setbuf(file,NULL); // disable buffering
		break;
	}

	if(timeout != 0) setEvent(0, (float)(timeout + 1));

	if(file==NULL) {
		LOG_MSG("Parallel %d: Failed to open %s",(int)port_nr+1,name.c_str());
		fileOpen = false;
		return false;
	} else {
		fileOpen = true;
		return true;
	}
}


bool CFileLPT::Putchar(uint8_t val)
{	
#if PARALLEL_DEBUG
	log_par(dbg_putchar,"putchar  0x%2x",val);
	if(dbg_plainputchar) fprintf(debugfp,"%c",val);
#endif
	
	// write to file (or not)
	lastUsedTick = PIC_Ticks;
	if(!fileOpen) {bufct = 0;if(!OpenFile()) return false;}
    if(bufct<100) bufput[bufct++]=val;

	if(codepage_ptr!=NULL) {
		uint16_t extchar = codepage_ptr[val];
		if(extchar & 0xFF00) fputc((int)((uint8_t)(extchar >> 8)),file);
		fputc((Bitu)(extchar & 0xFF),file);

	} else fputc((Bitu)val,file);
	if(addLF) {
		if((lastChar == 0x0d) && (val != 0x0a)) {
			fputc(0xa,file);
		}
		lastChar = val;
	}

	return true;
}
Bitu CFileLPT::Read_PR() {
	return datareg;
}
Bitu CFileLPT::Read_COM() {
	return controlreg;
}
Bitu CFileLPT::Read_SR() {
	uint8_t status =0x9f;
	if(!ack) status |= 0x40;
	ack=false;
	return status;
}

void CFileLPT::Write_PR(Bitu val) {
	datareg = (uint8_t)val;
}
void CFileLPT::Write_CON(Bitu val) {
	// init printer if bit 4 is switched on
	// ...
	autofeed = ((val & 0x02)!=0); // autofeed adds 0xa if 0xd is sent

	// data is strobed to the parallel printer on the falling edge of strobe bit
	if((!(val&0x1)) && (controlreg & 0x1)) {
		Putchar(datareg);
		if(autofeed && (datareg==0xd)) Putchar(0xa);
		ack = true;
	}
	controlreg=val&0xF; /* do NOT store bit 5, we do not emulate bidirectional LPT ports, yet */
}
void CFileLPT::Write_IOSEL(Bitu val) {
    (void)val;//UNUSED
	// not needed for file printing functionality
}
void CFileLPT::handleUpperEvent(uint16_t type) {
    (void)type;//UNUSED
	if(fileOpen && timeout != 0) {
		if(lastUsedTick + timeout < PIC_Ticks) {
			if(addFF) {
				fputc(12,file);
			}
			fclose(file);
			lastChar = 0;
			fileOpen=false;
			LOG_MSG("Parallel %d: File closed.",(int)port_nr+1);
            if (action1.size()||action2.size()||action3.size()) {
                bool isPCL = false;															// For now
                bool isPS = false;															// Postscript can be embedded (some WP drivers)
                if ((action1.size()||action2.size())&&bufct>5) {
                    if (!strncmp(bufput, sig1PCL, sizeof(sig1PCL)-1) || !strncmp(bufput, sig2PCL, sizeof(sig2PCL)-1)) {
                        isPCL = true;
                        int max = bufct>65?60:bufct-5;										// A line should start with the signature in the first 60 characters or so
                        for (int i = 0; i < max; i++)
                            if (!strncmp(bufput+i, sigPS, sizeof(sigPS)-1)) {
                                isPS = true;
                                break;
                            }
                    } else {																// Also test for PCL Esc sequence
                        if (!strncmp(bufput, sigPS+1, sizeof(sigPS)-2))
                            isPS = true;
                        char *p = bufput;
                        int count = bufct;
                        while (count-- > 1)
                            if (*(p++) == 0x1b)
                                if (*p == '@')												// <Esc>@ = Printer reset Epson
                                    break;
                                else if (*p > 0x24 && *p < 0x2b && isalpha(*(p+1))) {
                                    isPCL = true;
                                    break;
                                }
                    }
                }
                std::string action=action1.size()&&isPS?action1:(action2.size()&&isPCL?action2:action3);
                bool fail=false;
#if defined(WIN32)
                std::size_t found = action.find_first_of(" ");
                std::string para = name;
                if (found!=std::string::npos) {
                    para=action.substr(found+1)+" "+name;
                    action=action.substr(0, found);
                }
                fail=(INT_PTR)ShellExecute(NULL, "open", action.c_str(), para.c_str(), NULL, SW_SHOWNORMAL)<=32;
#else
                fail=system((action+" "+name).c_str())!=0;
#endif
                if (action4.size()&&fail) {
#if defined(WIN32)
                    std::size_t found = action4.find_first_of(" ");
                    para = name;
                    if (found!=std::string::npos) {
                        para=action4.substr(found+1)+" "+name;
                        action4=action4.substr(0, found);
                    }
                    fail=(INT_PTR)ShellExecute(NULL, "open", action4.c_str(), para.c_str(), NULL, SW_SHOWNORMAL)<=32;
#else
                    fail=system((action4+" "+name).c_str())!=0;
#endif
                }
                bool systemmessagebox(char const * aTitle, char const * aMessage, char const * aDialogType, char const * aIconType, int aDefaultButton);
                if (fail) systemmessagebox("Error", "The requested file printing handler failed to complete.", "ok","error", 1);
            }
            bufct = 0;
		} else {
			// Port has been touched in the meantime, try again later
			float new_delay = (float)((timeout + 1) - (PIC_Ticks - lastUsedTick));
			setEvent(0, new_delay);
		}
	}
}

