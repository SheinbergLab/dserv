#if 0
********************************************************************************
Project		Baylor College of Medicine/MPI
Program 	evt_name.h
Date		1997Feb19(Wed)/2024Jun13(Thurs)
Author		Dean Z. Douthat/David Sheinberg
Description	Pre-assigned numbers, enums, names and timestamp types
================================================================================
$Locker:  $
$Log: evt_name.h,v $
Revision 1.20  2003/09/20 22:08:44  sheinb
Changed param type for spikes to PUT_long

Revision 1.19  1998/07/23 00:39:59  sheinb
Added E_STIMULATOR

Revision 1.18  1998/07/19 00:03:39  sheinb
Added evt for MRI

Revision 1.17  1998/07/06 00:51:55  sheinb
Added E_PHYS event (45)

Revision 1.16  1998/04/21 10:00:06  sheinb
Added E_PARAM as eventtype 6 (type string)

Revision 1.15  1997/12/26 12:16:57  sheinb
Fixed type in em_log

Revision 1.14  1997/03/25 21:37:23  sheinb
Renamed E_NAME (duplicate) to E_ID

Revision 1.13  1997/03/25 21:36:29  sheinb
Removed subject event but made experiment name event more general

Revision 1.12  1997/03/25 21:32:20  sheinb
Added E_SUBJECT event for subject name

Revision 1.11  1997/03/24 00:41:50  sheinb
Made fixspot evt type PUT_float

Revision 1.10  1997/03/16 21:13:59  sheinb
More mods to E_EMPARMS

Revision 1.9  1997/03/16 21:06:13  sheinb
Changed EM_REGION to EM_PARMS and made datatype float

Revision 1.8  1997/03/06 23:59:57  sheinb
Changed a few more group names

Revision 1.7  1997/03/06 01:18:48  sheinb
Added E_SOUND event

Revision 1.6  1997/03/06 00:40:00  sheinb
Added system messages

Revision 1.5  1997/03/05 01:24:08  sheinb
Changed trace event to type PUT_string

Revision 1.4  1997/03/05 01:10:20  sheinb
Capitalized event name (E_FILE ...)

Revision 1.3  1997/03/05 00:13:57  dean
revise to new style file and name handling

Revision 1.2  1997/03/02 01:35:48  dean
add timestamp time and enum for name, add group

*******************************************************************************
#endif
/*****************************************************************************/
/***************************** EVENT DEFINITIONS *****************************/
/*****************************************************************************/
//	ndx	enum	    name					  timestamp put_type
name(0,	E_MAGIC,     Magic Number,			    'c', PUT_null)
name(1,	E_NAME,	     Event Name,			    'c', PUT_string)
//subtype is event to change, 1st stamp char is timestamp type, 2nd is PUT_type

name(2,	E_FILE,    	 File I/O,	        	    'c', PUT_string)
//ndata 0, close only: subtype 0, truncate any existing file;
//subtype 1, retain existing file; subtype 2, append to existing file

name(3,	E_USER,	     User Interaction,          'c', PUT_null)
name(4,	E_TRACE,     State System Trace,	    'c', PUT_string) 
name(5,	E_PARAM,     Parameter Set,	            'c', PUT_string) 
//6-15 Reserved

name(16,E_FSPIKE,    Time Stamped Spike,	    'c', PUT_long)
name(17,E_HSPIKE,    DIS-1 Hardware Spike,      'c', PUT_long)

name(18, E_ID,       Name,                      'c', PUT_string)		 

name(19, E_BEGINOBS, Start Obs Period,          'c', PUT_long)
name(20, E_ENDOBS,   End Obs Period,            'c', PUT_long)

name(21, E_ISI,      ISI,                       'c', PUT_long)
name(22, E_TRIALTYPE,Trial Type,                'c', PUT_long)
name(23, E_OBSTYPE,  Obs Period Type,           'c', PUT_long)

name(24, E_EMLOG,    EM Log,                    'c', PUT_long)
name(25, E_FIXSPOT,  Fixspot,                   'c', PUT_float)
name(26, E_EMPARAMS, EM Params,                 'c', PUT_float)

name(27, E_STIMULUS, Stimulus,                  'c', PUT_long)
name(28, E_PATTERN,  Pattern,                   'c', PUT_long)
name(29, E_STIMTYPE, Stimulus Type,             'c', PUT_long)
name(30, E_SAMPLE,   Sample,                    'c', PUT_long)
name(31, E_PROBE,    Probe,                     'c', PUT_long) 
name(32, E_CUE,      Cue,                       'c', PUT_long)
name(33, E_TARGET,   Target,                    'c', PUT_long)
name(34, E_DISTRACTOR, Distractor,              'c', PUT_long)

name(35, E_SOUND,    Sound Event,               'c', PUT_long)
	 
name(36, E_FIXATE,   Fixation,                  'c', PUT_long)
name(37, E_RESP,     Response,                  'c', PUT_long)
name(38, E_SACCADE,  Saccade,                   'c', PUT_long)
name(39, E_DECIDE,   Decide,                    'c', PUT_long)

name(40, E_ENDTRIAL, EOT,                       'c', PUT_long)
name(41, E_ABORT,    Abort,                     'c', PUT_long)
name(42, E_REWARD,   Reward,                    'c', PUT_long)
name(43, E_DELAY,    Delay,                     'c', PUT_long)
name(44, E_PUNISH,   Punish,                    'c', PUT_long)

name(45, E_PHYS,     Physio Params,             'c', PUT_float)
name(46, E_MRI,      Mri,                       'c', PUT_long)


name(47, E_STIMULATOR, Stimulator Signal,       'c', PUT_long)

//48-127		System events
//128-255		User events
name(128, E_TARGNAME, Target Name, 'c', PUT_string)
name(129, E_SCENENAME, Scene Name, 'c', PUT_string) 
name(130, E_SACCADE_INFO, Saccade Data, 'c', PUT_float) 
name(131, E_STIM_TRIGGER, Stimulus Trigger, 'c', PUT_float)
name(132, E_MOVIENAME, Movie Name, 'c', PUT_string)
name(133, E_STIMULATION, Electrical Stimulation, 'c', PUT_long)
name(134, E_SECOND_CHANCE, Second Chance, 'c', PUT_long)
name(135, E_SECOND_RESP, Second Response, 'c', PUT_long)
name(136, E_SWAPBUFFER, Swap Buffer, 'c', PUT_float)
name(137, E_STIM_DATA, Stim Data, 'c', PUT_string)
name(138, E_DIGITAL_LINES, Digital Input Status, 'c', PUT_long)
				    
/*****************************************************************************/
/******************************* EVENT GROUPS ********************************/
/*****************************************************************************/
//Note: group 0 reserved -- it means no group registration, just individual ones
//	group 1 reserved -- it means register for all events

//    group		min	max
//   number		event	event
group(2,		0, 	15)	//system events
group(3,		18,	127)	//predefined events
group(4,		128,	255)	//user events
