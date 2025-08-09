/*************************************************************************
 *
 *  NAME
 *    df.h
 *
 *  DESCRIPTION
 *    Structure definitions for unified datafile format for storing
 *  behavioral and spike data.
 *
 *  NOTES
 *    New structures and new structure entries *CAN* be added to this 
 *  file.  They can physically appear anywhere in the structure, however
 *  their struct_id/tags must be added at the *END* of the enum lists.
 *  This ensures that the arbitrary numbers which serve as "opcodes" are
 *  always compatible with previous versions of data files.
 *
 *  AUTHOR
 *    DLS
 *
 ************************************************************************/

#ifndef _DF_H_
#define _DF_H_


#define DF_ASCII  1
#define DF_BINARY 2
#define DF_LZ4 3

#define DF_OK       1
#define DF_FINISHED 2
#define DF_ABORT    3

extern float dfVersion;		/* to keep track of different versions */

#define DF_MAGIC_NUMBER_SIZE 4 
extern char dfMagicNumber[];	/* to uniquely identify this file type */

/*
 * Data types which are used in the storage of structures
 */
enum DATA_TYPE {
  DF_VERSION,                   /* used to figure out byte ordering    */
  DF_FLAG, DF_CHAR, DF_LONG, DF_SHORT, DF_FLOAT, DF_STRUCTURE, 
  DF_STRING, DF_LONG_ARRAY, DF_SHORT_ARRAY, DF_FLOAT_ARRAY,
  DF_STRING_ARRAY, DF_LIST, DF_VOID, DF_VOID_ARRAY, DF_CHAR_ARRAY, 
  DF_LIST_ARRAY
};

typedef struct _tag_info {
  int   tag_id;			/* enumerated tag identifier           */
  char *tag_name;		/* name to print out when dumped       */
  int   data_type;		/* enumerated data type                */
  int   struct_type;		/* either TOP_LEVEL or some struct     */
} TAG_INFO;

/***********************************************************************
 *
 *  Structures which are described here and are dumpable/readable:
 *      DATA_FILE
 *         DF_INFO
 *         OBS_P
 *            OBS_INFO
 *            EV_DATA
 *               EV_LIST
 *            EM_DATA
 *            SP_DATA
 *               SP_CH_DATA
 *          CELL_INFO
 *
 ***********************************************************************/

enum STRUCT_TYPE {
  TOP_LEVEL,			/* regular data type (e.g. long, float)*/
  DATA_FILE_STRUCT, 
  DF_INFO_STRUCT, 
  OBS_P_STRUCT, 
  OBS_INFO_STRUCT, 
  EV_DATA_STRUCT, 
  EV_LIST_STRUCT, 
  EM_DATA_STRUCT, 
  SP_DATA_STRUCT, 
  SP_CHANNEL_STRUCT,
  CELL_INFO_STRUCT,
  N_STRUCT_TYPES		/* leave as last struct                */
};

/*
 * The only TAGS which can appear before one of the defined structures
 * are the following.  These contain info about the datafile version and
 * then enter the highest level structure.
 */

enum TOP_LEVEL_TAGS { T_VERSION_TAG, T_BEGIN_DF_TAG };

/*
 * Opcode reserved to indicate the end of a structure
 */
#define END_STRUCT 255		/* tag indicating end of current struct   */

/***********************************************************************
 *
 *   Structure: OBS_INFO
 *   Refers to: None
 *   Found in:  OBS_P
 *   Purpose:   Information about the specific observation period
 *
 ***********************************************************************/

enum OBS_INFO_TAG { 
  O_BLOCK_TAG, O_OBSP_TAG, O_STATUS_TAG, O_DURATION_TAG, O_NTRIALS_TAG,
  O_FILENUM_TAG, O_INDEX_TAG
};

typedef struct {
  int filenum;			/* which file from a group this is from*/
  int index;			/* just a sequential index for a file  */
  int block;			/* block identifier                    */
  int obsp;			/* observation period identifier       */
  int status;			/* "EOT" status                        */
  int duration;			/* duration of the block in msecs      */
  int ntrials;			/* number of "trials" in the obsp      */
} OBS_INFO;

#define OBS_FILENUM(o)         ((o)->filenum)
#define OBS_INDEX(o)           ((o)->index)
#define OBS_BLOCK(o)           ((o)->block)
#define OBS_OBSP(o)            ((o)->obsp)
#define OBS_STATUS(o)          ((o)->status)
#define OBS_DURATION(o)        ((o)->duration)
#define OBS_NTRIALS(o)         ((o)->ntrials)


/***********************************************************************
 *
 *   Structure: EV_LIST
 *   Refers to: None
 *   Found in:  EV_DATA
 *   Purpose:   Maintain a list of one event type
 *
 ***********************************************************************/

enum EV_LIST_TAG { E_VAL_LIST_TAG, E_TIME_LIST_TAG };

typedef struct {
  int n;
  int ntimes;
  int *vals;
  int *times;
} EV_LIST;

#define EV_LIST_N(e)         ((e)->n)
#define EV_LIST_NTIMES(e)    ((e)->ntimes)
#define EV_LIST_VALS(e)      ((e)->vals)
#define EV_LIST_TIMES(e)     ((e)->times)

#define EV_LIST_VAL(e,i)     (EV_LIST_VALS(e)[i])
#define EV_LIST_TIME(e,i)    (EV_LIST_TIMES(e)[i])


/***********************************************************************
 *
 *   Structure: EV_DATA
 *   Refers to: EV_LIST
 *   Found in:  OBS_P
 *   Purpose:   Maintain a list of all events for an observation period
 *
 ***********************************************************************/

enum EV_DATA_TAG { 
  E_FIXON_TAG, E_FIXOFF_TAG, E_STIMON_TAG, E_STIMOFF_TAG, 
  E_RESP_TAG,  E_PATON_TAG, E_PATOFF_TAG, 
  E_STIMTYPE_TAG, E_PATTERN_TAG, E_REWARD_TAG,
  E_PROBEON_TAG, E_PROBEOFF_TAG, E_SAMPON_TAG, E_SAMPOFF_TAG, 
  E_FIXATE_TAG, E_DECIDE_TAG, E_STIMULUS_TAG, E_DELAY_TAG,
  E_ISI_TAG, E_UNIT_TAG, E_INFO_TAG, E_CUE_TAG, E_TARGET_TAG,
  E_DISTRACTOR_TAG, E_CORRECT_TAG, E_TRIALTYPE_TAG,
  E_ABORT_TAG, E_WRONG_TAG, E_PUNISH_TAG,
  E_BLANKING_TAG, E_SACCADE_TAG, 
  E_NEVENT_TAGS		/* leave at end as counter */
};

typedef struct {
  EV_LIST fixon;
  EV_LIST fixoff;
  EV_LIST stimon;
  EV_LIST stimoff;
  EV_LIST resp;
  EV_LIST paton;
  EV_LIST patoff;
  EV_LIST stimtype;
  EV_LIST pattern;
  EV_LIST reward;
  EV_LIST probeon;
  EV_LIST probeoff;
  EV_LIST sampon;
  EV_LIST sampoff;
  EV_LIST fixate;
  EV_LIST decide;
  EV_LIST stimulus;
  EV_LIST delay;
  EV_LIST isi;
  EV_LIST unit;
  EV_LIST info;
  EV_LIST cue;
  EV_LIST target;
  EV_LIST distractor;
  EV_LIST correct;
  EV_LIST trialtype;
  EV_LIST abort;
  EV_LIST wrong;
  EV_LIST punish;
  EV_LIST blanking;
  EV_LIST saccade;
} EV_DATA;

#define EV_FIXON(e)            (&(e)->fixon)
#define EV_FIXOFF(e)           (&(e)->fixoff)
#define EV_STIMON(e)           (&(e)->stimon)
#define EV_STIMOFF(e)          (&(e)->stimoff)
#define EV_RESP(e)             (&(e)->resp)
#define EV_PATON(e)            (&(e)->paton)
#define EV_PATOFF(e)           (&(e)->patoff)
#define EV_STIMTYPE(e)         (&(e)->stimtype)
#define EV_PATTERN(e)          (&(e)->pattern)
#define EV_REWARD(e)           (&(e)->reward)
#define EV_PROBEON(e)          (&(e)->probeon)
#define EV_PROBEOFF(e)         (&(e)->probeoff)
#define EV_SAMPON(e)           (&(e)->sampon)
#define EV_SAMPOFF(e)          (&(e)->sampoff)
#define EV_FIXATE(e)           (&(e)->fixate)
#define EV_DECIDE(e)           (&(e)->decide)
#define EV_STIMULUS(e)         (&(e)->stimulus)
#define EV_DELAY(e)            (&(e)->delay)
#define EV_ISI(e)              (&(e)->isi)
#define EV_UNIT(e)             (&(e)->unit)
#define EV_INFO(e)             (&(e)->info)
#define EV_CUE(e)              (&(e)->cue)
#define EV_TARGET(e)           (&(e)->target)
#define EV_DISTRACTOR(e)       (&(e)->distractor)
#define EV_CORRECT(e)          (&(e)->correct)
#define EV_TRIALTYPE(e)        (&(e)->trialtype)
#define EV_ABORT(e)            (&(e)->abort)
#define EV_WRONG(e)            (&(e)->wrong)
#define EV_PUNISH(e)           (&(e)->punish)
#define EV_BLANKING(e)         (&(e)->blanking)
#define EV_SACCADE(e)          (&(e)->saccade)



/***********************************************************************
 *
 *   Structure: EM_INFO
 *   Refers to: None
 *   Found in:  OBS_P
 *   Purpose:   Eye movement data for an observation period
 *
 ***********************************************************************/

enum EM_DATA_TAG { 
  E_ONTIME_TAG, E_RATE_TAG, E_FIXPOS_TAG, E_WINDOW_TAG, 
  E_PNT_DEG_TAG, E_H_EM_LIST_TAG, E_V_EM_LIST_TAG, E_WINDOW2_TAG
};

typedef struct {
  int ontime;			/* time after obsp when em samps start */
  float rate;			/* how often, in ms, emsamps occur     */
  short fixpos[2];		/* H & V position corresp. to fixpos   */
  short window[4];		/* em window positions                 */
  short window2[4];		/* em refixate window positions        */
  int pnt_deg;			/* number of ADC points per degree va  */
  int nemsamps;	        	/* number of emsamps for this obs per  */
  short *emsamps_h;		/* horizontal eye movement samples     */
  short *emsamps_v;		/* vertical eye movement samples       */
} EM_DATA;


#define EM_ONTIME(e)       ((e)->ontime)
#define EM_RATE(e)         ((e)->rate)
#define EM_FIXPOS(e)       ((e)->fixpos)
#define EM_WINDOW(e)       ((e)->window)
#define EM_WINDOW2(e)      ((e)->window2)
#define EM_PNT_DEG(e)      ((e)->pnt_deg)
#define EM_NSAMPS(e)       ((e)->nemsamps)
#define EM_SAMPS_H(e)      ((e)->emsamps_h)
#define EM_SAMPS_V(e)      ((e)->emsamps_v)


/***********************************************************************
 *
 *   Structure: SP_CH_DATA
 *   Refers to: None
 *   Found in:  SP_DATA
 *   Purpose:   Contains spike data for one channel
 *
 *   NOTE:  Spike "image" data will someday be added to this structure
 *
 ***********************************************************************/

enum SP_CH_DATA_TAG { S_CH_DATA_TAG, S_CH_SOURCE_TAG, S_CH_CELLNUM_TAG };
enum SOURCE_IDS { SOURCE_PDP, SOURCE_HIST };

typedef struct {
  char source;
  int  cellnum;
  int nsptimes;
  float *sptimes;
} SP_CH_DATA;

#define SP_CH_SOURCE(s)          ((s)->source)
#define SP_CH_CELLNUM(s)         ((s)->cellnum)
#define SP_CH_NSPTIMES(s)        ((s)->nsptimes)
#define SP_CH_SPTIMES(s)         ((s)->sptimes)

/***********************************************************************
 *
 *   Structure: SP_DATA
 *   Refers to: SP_CH_DATA
 *   Found in:  OBS_P
 *   Purpose:   Contains all spike data channels for an obs period
 *
 ***********************************************************************/

enum SP_DATA_TAG { S_NCHANNELS_TAG, S_CHANNEL_TAG };

typedef struct {
  int nchannels;
  SP_CH_DATA *channels;
} SP_DATA;

#define SP_NCHANNELS(s)        ((s)->nchannels)
#define SP_CHANNELS(s)         ((s)->channels)
#define SP_CHANNEL(s,i)        (&SP_CHANNELS(s)[i])

/***********************************************************************
 *
 *   Structure: OBS_P
 *   Refers to: OBS_INFO, EV_DATA, EM_DATA, SP_DATA 
 *   Found in:  DATA_FILE
 *   Purpose:   Contains all info about the observation period
 *
 ***********************************************************************/

enum OBS_P_TAG {
  O_INFO_TAG, O_EVDATA_TAG, O_SPDATA_TAG, O_EMDATA_TAG
};

typedef struct {
  OBS_INFO info;
  EV_DATA  evdata;
  SP_DATA  spdata;
  EM_DATA  emdata;
} OBS_P;

#define OBSP_INFO(o)           (&(o)->info)
#define OBSP_EVDATA(o)         (&(o)->evdata)
#define OBSP_SPDATA(o)         (&(o)->spdata)
#define OBSP_EMDATA(o)         (&(o)->emdata)


/**********************************************************************
 * 
 * DEFINES for accessing single events from OBSPeriods
 *
 **********************************************************************/

#define O_FIXON_N(o)            (EV_LIST_N(EV_FIXON(OBSP_EVDATA(o))))
#define O_FIXOFF_N(o)           (EV_LIST_N(EV_FIXOFF(OBSP_EVDATA(o))))
#define O_STIMON_N(o)           (EV_LIST_N(EV_STIMON(OBSP_EVDATA(o))))
#define O_STIMOFF_N(o)          (EV_LIST_N(EV_STIMOFF(OBSP_EVDATA(o))))
#define O_RESP_N(o)             (EV_LIST_N(EV_RESP(OBSP_EVDATA(o))))
#define O_PATON_N(o)            (EV_LIST_N(EV_PATON(OBSP_EVDATA(o))))
#define O_PATOFF_N(o)           (EV_LIST_N(EV_PATOFF(OBSP_EVDATA(o))))
#define O_STIMTYPE_N(o)         (EV_LIST_N(EV_STIMTYPE(OBSP_EVDATA(o))))
#define O_PATTERN_N(o)          (EV_LIST_N(EV_PATTERN(OBSP_EVDATA(o))))
#define O_REWARD_N(o)           (EV_LIST_N(EV_REWARD(OBSP_EVDATA(o))))
#define O_PROBEON_N(o)          (EV_LIST_N(EV_PROBEON(OBSP_EVDATA(o))))
#define O_PROBEOFF_N(o)         (EV_LIST_N(EV_PROBEOFF(OBSP_EVDATA(o))))
#define O_SAMPON_N(o)           (EV_LIST_N(EV_SAMPON(OBSP_EVDATA(o))))
#define O_SAMPOFF_N(o)          (EV_LIST_N(EV_SAMPOFF(OBSP_EVDATA(o))))
#define O_FIXATE_N(o)           (EV_LIST_N(EV_FIXATE(OBSP_EVDATA(o))))
#define O_DECIDE_N(o)           (EV_LIST_N(EV_DECIDE(OBSP_EVDATA(o))))
#define O_STIMULUS_N(o)         (EV_LIST_N(EV_STIMULUS(OBSP_EVDATA(o))))
#define O_DELAY_N(o)            (EV_LIST_N(EV_DELAY(OBSP_EVDATA(o))))
#define O_ISI_N(o)              (EV_LIST_N(EV_ISI(OBSP_EVDATA(o))))
#define O_UNIT_N(o)             (EV_LIST_N(EV_UNIT(OBSP_EVDATA(o))))
#define O_CUE_N(o)              (EV_LIST_N(EV_CUE(OBSP_EVDATA(o))))
#define O_TARGET_N(o)           (EV_LIST_N(EV_TARGET(OBSP_EVDATA(o))))
#define O_DISTRACTOR_N(o)       (EV_LIST_N(EV_DISTRACTOR(OBSP_EVDATA(o))))
#define O_CORRECT_N(o)          (EV_LIST_N(EV_CORRECT(OBSP_EVDATA(o))))
#define O_ABORT_N(o)            (EV_LIST_N(EV_ABORT(OBSP_EVDATA(o))))
#define O_BLANKING_N(o)         (EV_LIST_N(EV_BLANKING(OBSP_EVDATA(o))))
#define O_PUNISH_N(o)           (EV_LIST_N(EV_PUNISH(OBSP_EVDATA(o))))
#define O_WRONG_N(o)            (EV_LIST_N(EV_WRONG(OBSP_EVDATA(o))))
#define O_SACCADE_N(o)          (EV_LIST_N(EV_SACCADE(OBSP_EVDATA(o))))

/* 
 * Events with parameters use NTIMES to count number of events
 */
#define O_INFO_N(o)             (EV_LIST_NTIMES(EV_INFO(OBSP_EVDATA(o))))

#define O_FIXON_V(o)            (EV_LIST_VAL(EV_FIXON(OBSP_EVDATA(o)),0))
#define O_FIXON_T(o)            (EV_LIST_TIME(EV_FIXON(OBSP_EVDATA(o)),0))
#define O_FIXOFF_V(o)           (EV_LIST_VAL(EV_FIXOFF(OBSP_EVDATA(o)),0))
#define O_FIXOFF_T(o)           (EV_LIST_TIME(EV_FIXOFF(OBSP_EVDATA(o)),0))
#define O_STIMON_V(o)           (EV_LIST_VAL(EV_STIMON(OBSP_EVDATA(o)),0))
#define O_STIMON_T(o)           (EV_LIST_TIME(EV_STIMON(OBSP_EVDATA(o)),0))
#define O_STIMOFF_V(o)          (EV_LIST_VAL(EV_STIMOFF(OBSP_EVDATA(o)),0))
#define O_STIMOFF_T(o)          (EV_LIST_TIME(EV_STIMOFF(OBSP_EVDATA(o)),0))
#define O_RESP_V(o)             (EV_LIST_VAL(EV_RESP(OBSP_EVDATA(o)),0))
#define O_RESP_T(o)             (EV_LIST_TIME(EV_RESP(OBSP_EVDATA(o)),0))
#define O_PATON_V(o)            (EV_LIST_VAL(EV_PATON(OBSP_EVDATA(o)),0))
#define O_PATON_T(o)            (EV_LIST_TIME(EV_PATON(OBSP_EVDATA(o)),0))
#define O_PATOFF_V(o)           (EV_LIST_VAL(EV_PATOFF(OBSP_EVDATA(o)),0))
#define O_PATOFF_T(o)           (EV_LIST_TIME(EV_PATOFF(OBSP_EVDATA(o)),0))
#define O_STIMTYPE_V(o)         (EV_LIST_VAL(EV_STIMTYPE(OBSP_EVDATA(o)),0))
#define O_STIMTYPE_T(o)         (EV_LIST_TIME(EV_STIMTYPE(OBSP_EVDATA(o)),0))
#define O_PATTERN_V(o)          (EV_LIST_VAL(EV_PATTERN(OBSP_EVDATA(o)),0))
#define O_PATTERN_T(o)          (EV_LIST_TIME(EV_PATTERN(OBSP_EVDATA(o)),0))
#define O_REWARD_V(o)           (EV_LIST_VAL(EV_REWARD(OBSP_EVDATA(o)),0))
#define O_REWARD_T(o)           (EV_LIST_TIME(EV_REWARD(OBSP_EVDATA(o)),0))
#define O_PROBEON_V(o)          (EV_LIST_VAL(EV_PROBEON(OBSP_EVDATA(o)),0))
#define O_PROBEON_T(o)          (EV_LIST_TIME(EV_PROBEON(OBSP_EVDATA(o)),0))
#define O_PROBEOFF_V(o)         (EV_LIST_VAL(EV_PROBEOFF(OBSP_EVDATA(o)),0))
#define O_PROBEOFF_T(o)         (EV_LIST_TIME(EV_PROBEOFF(OBSP_EVDATA(o)),0))
#define O_SAMPON_V(o)           (EV_LIST_VAL(EV_SAMPON(OBSP_EVDATA(o)),0))
#define O_SAMPON_T(o)           (EV_LIST_TIME(EV_SAMPON(OBSP_EVDATA(o)),0))
#define O_SAMPOFF_V(o)          (EV_LIST_VAL(EV_SAMPOFF(OBSP_EVDATA(o)),0))
#define O_SAMPOFF_T(o)          (EV_LIST_TIME(EV_SAMPOFF(OBSP_EVDATA(o)),0))
#define O_FIXATE_V(o)           (EV_LIST_VAL(EV_FIXATE(OBSP_EVDATA(o)),0))
#define O_FIXATE_T(o)           (EV_LIST_TIME(EV_FIXATE(OBSP_EVDATA(o)),0))
#define O_DECIDE_V(o)           (EV_LIST_VAL(EV_DECIDE(OBSP_EVDATA(o)),0))
#define O_DECIDE_T(o)           (EV_LIST_TIME(EV_DECIDE(OBSP_EVDATA(o)),0))
#define O_STIMULUS_V(o)         (EV_LIST_VAL(EV_STIMULUS(OBSP_EVDATA(o)),0))
#define O_STIMULUS_T(o)         (EV_LIST_TIME(EV_STIMULUS(OBSP_EVDATA(o)),0))
#define O_DELAY_V(o)            (EV_LIST_VAL(EV_DELAY(OBSP_EVDATA(o)),0))
#define O_DELAY_T(o)            (EV_LIST_TIME(EV_DELAY(OBSP_EVDATA(o)),0))
#define O_ISI_V(o)              (EV_LIST_VAL(EV_ISI(OBSP_EVDATA(o)),0))
#define O_ISI_T(o)              (EV_LIST_TIME(EV_ISI(OBSP_EVDATA(o)),0))
#define O_CUE_V(o)              (EV_LIST_VAL(EV_CUE(OBSP_EVDATA(o)),0))
#define O_CUE_T(o)              (EV_LIST_TIME(EV_CUE(OBSP_EVDATA(o)),0))
#define O_TARGET_V(o)           (EV_LIST_VAL(EV_TARGET(OBSP_EVDATA(o)),0))
#define O_TARGET_T(o)           (EV_LIST_TIME(EV_TARGET(OBSP_EVDATA(o)),0))
#define O_DISTRACTOR_V(o)       (EV_LIST_VAL(EV_DISTRACTOR(OBSP_EVDATA(o)),0))
#define O_DISTRACTOR_T(o)       (EV_LIST_TIME(EV_DISTRACTOR(OBSP_EVDATA(o)),0))
#define O_CORRECT_V(o)          (EV_LIST_VAL(EV_CORRECT(OBSP_EVDATA(o)),0))
#define O_CORRECT_T(o)          (EV_LIST_TIME(EV_CORRECT(OBSP_EVDATA(o)),0))
#define O_UNIT_V(o)             (EV_LIST_VAL(EV_UNIT(OBSP_EVDATA(o)),0))
#define O_UNIT_T(o)             (EV_LIST_TIME(EV_UNIT(OBSP_EVDATA(o)),0))
#define O_ABORT_V(o)            (EV_LIST_VAL(EV_ABORT(OBSP_EVDATA(o)),0))
#define O_ABORT_T(o)            (EV_LIST_TIME(EV_ABORT(OBSP_EVDATA(o)),0))
#define O_BLANKING_V(o)         (EV_LIST_VAL(EV_BLANKING(OBSP_EVDATA(o)),0))
#define O_BLANKING_T(o)         (EV_LIST_TIME(EV_BLANKING(OBSP_EVDATA(o)),0))
#define O_PUNISH_V(o)           (EV_LIST_VAL(EV_PUNISH(OBSP_EVDATA(o)),0))
#define O_PUNISH_T(o)           (EV_LIST_TIME(EV_PUNISH(OBSP_EVDATA(o)),0))
#define O_WRONG_V(o)            (EV_LIST_VAL(EV_WRONG(OBSP_EVDATA(o)),0))
#define O_WRONG_T(o)            (EV_LIST_TIME(EV_WRONG(OBSP_EVDATA(o)),0))
#define O_SACCADE_V(o)          (EV_LIST_VAL(EV_SACCADE(OBSP_EVDATA(o)),0))
#define O_SACCADE_T(o)          (EV_LIST_TIME(EV_SACCADE(OBSP_EVDATA(o)),0))

#define O_INFO_T(o)             (EV_LIST_TIME(EV_INFO(OBSP_EVDATA(o)),0))
#define O_INFO_V(o)             (&(EV_LIST_VAL(EV_INFO(OBSP_EVDATA(o)),0)))

#define O_NSPIKES(o)            SP_CH_NSPTIMES(SP_CHANNEL(OBSP_SPDATA(o),0))
#define O_SPIKES(o)             SP_CH_SPTIMES(SP_CHANNEL(OBSP_SPDATA(o),0))
#define O_SPIKE(o,i)            SP_CH_SPTIMES(SP_CHANNEL(OBSP_SPDATA(o),0))[i]

#define O_N_FIXON_V(o,n)        (EV_LIST_VAL(EV_FIXON(OBSP_EVDATA(o)),n))
#define O_N_FIXON_T(o,n)        (EV_LIST_TIME(EV_FIXON(OBSP_EVDATA(o)),n))
#define O_N_FIXOFF_V(o,n)       (EV_LIST_VAL(EV_FIXOFF(OBSP_EVDATA(o)),n))
#define O_N_FIXOFF_T(o,n)       (EV_LIST_TIME(EV_FIXOFF(OBSP_EVDATA(o)),n))
#define O_N_STIMON_V(o,n)       (EV_LIST_VAL(EV_STIMON(OBSP_EVDATA(o)),n))
#define O_N_STIMON_T(o,n)       (EV_LIST_TIME(EV_STIMON(OBSP_EVDATA(o)),n))
#define O_N_STIMOFF_V(o,n)      (EV_LIST_VAL(EV_STIMOFF(OBSP_EVDATA(o)),n))
#define O_N_STIMOFF_T(o,n)      (EV_LIST_TIME(EV_STIMOFF(OBSP_EVDATA(o)),n))
#define O_N_RESP_V(o,n)         (EV_LIST_VAL(EV_RESP(OBSP_EVDATA(o)),n))
#define O_N_RESP_T(o,n)         (EV_LIST_TIME(EV_RESP(OBSP_EVDATA(o)),n))
#define O_N_PATON_V(o,n)        (EV_LIST_VAL(EV_PATON(OBSP_EVDATA(o)),n))
#define O_N_PATON_T(o,n)        (EV_LIST_TIME(EV_PATON(OBSP_EVDATA(o)),n))
#define O_N_PATOFF_V(o,n)       (EV_LIST_VAL(EV_PATOFF(OBSP_EVDATA(o)),n))
#define O_N_PATOFF_T(o,n)       (EV_LIST_TIME(EV_PATOFF(OBSP_EVDATA(o)),n))
#define O_N_STIMTYPE_V(o,n)     (EV_LIST_VAL(EV_STIMTYPE(OBSP_EVDATA(o)),n))
#define O_N_STIMTYPE_T(o,n)     (EV_LIST_TIME(EV_STIMTYPE(OBSP_EVDATA(o)),n))
#define O_N_PATTERN_V(o,n)      (EV_LIST_VAL(EV_PATTERN(OBSP_EVDATA(o)),n))
#define O_N_PATTERN_T(o,n)      (EV_LIST_TIME(EV_PATTERN(OBSP_EVDATA(o)),n))
#define O_N_REWARD_V(o,n)       (EV_LIST_VAL(EV_REWARD(OBSP_EVDATA(o)),n))
#define O_N_REWARD_T(o,n)       (EV_LIST_TIME(EV_REWARD(OBSP_EVDATA(o)),n))
#define O_N_PROBEON_V(o,n)      (EV_LIST_VAL(EV_PROBEON(OBSP_EVDATA(o)),n))
#define O_N_PROBEON_T(o,n)      (EV_LIST_TIME(EV_PROBEON(OBSP_EVDATA(o)),n))
#define O_N_PROBEOFF_V(o,n)     (EV_LIST_VAL(EV_PROBEOFF(OBSP_EVDATA(o)),n))
#define O_N_PROBEOFF_T(o,n)     (EV_LIST_TIME(EV_PROBEOFF(OBSP_EVDATA(o)),n))
#define O_N_SAMPON_V(o,n)       (EV_LIST_VAL(EV_SAMPON(OBSP_EVDATA(o)),n))
#define O_N_SAMPON_T(o,n)       (EV_LIST_TIME(EV_SAMPON(OBSP_EVDATA(o)),n))
#define O_N_SAMPOFF_V(o,n)      (EV_LIST_VAL(EV_SAMPOFF(OBSP_EVDATA(o)),n))
#define O_N_SAMPOFF_T(o,n)      (EV_LIST_TIME(EV_SAMPOFF(OBSP_EVDATA(o)),n))
#define O_N_FIXATE_V(o,n)       (EV_LIST_VAL(EV_FIXATE(OBSP_EVDATA(o)),n))
#define O_N_FIXATE_T(o,n)       (EV_LIST_TIME(EV_FIXATE(OBSP_EVDATA(o)),n))
#define O_N_DECIDE_V(o,n)       (EV_LIST_VAL(EV_DECIDE(OBSP_EVDATA(o)),n))
#define O_N_DECIDE_T(o,n)       (EV_LIST_TIME(EV_DECIDE(OBSP_EVDATA(o)),n))
#define O_N_STIMULUS_V(o,n)     (EV_LIST_VAL(EV_STIMULUS(OBSP_EVDATA(o)),n))
#define O_N_STIMULUS_T(o,n)     (EV_LIST_TIME(EV_STIMULUS(OBSP_EVDATA(o)),n))
#define O_N_DELAY_V(o,n)        (EV_LIST_VAL(EV_DELAY(OBSP_EVDATA(o)),n))
#define O_N_DELAY_T(o,n)        (EV_LIST_TIME(EV_DELAY(OBSP_EVDATA(o)),n))
#define O_N_ISI_V(o,n)          (EV_LIST_VAL(EV_ISI(OBSP_EVDATA(o)),n))
#define O_N_ISI_T(o,n)          (EV_LIST_TIME(EV_ISI(OBSP_EVDATA(o)),n))
#define O_N_CUE_V(o,n)          (EV_LIST_VAL(EV_CUE(OBSP_EVDATA(o)),n))
#define O_N_CUE_T(o,n)          (EV_LIST_TIME(EV_CUE(OBSP_EVDATA(o)),n))
#define O_N_TARGET_V(o,n)       (EV_LIST_VAL(EV_TARGET(OBSP_EVDATA(o)),n))
#define O_N_TARGET_T(o,n)       (EV_LIST_TIME(EV_TARGET(OBSP_EVDATA(o)),n))
#define O_N_DISTRACTOR_V(o,n)   (EV_LIST_VAL(EV_DISTRACTOR(OBSP_EVDATA(o)),n))
#define O_N_DISTRACTOR_T(o,n)   (EV_LIST_TIME(EV_DISTRACTOR(OBSP_EVDATA(o)),n))
#define O_N_CORRECT_V(o,n)      (EV_LIST_VAL(EV_CORRECT(OBSP_EVDATA(o)),n))
#define O_N_CORRECT_T(o,n)      (EV_LIST_TIME(EV_CORRECT(OBSP_EVDATA(o)),n))
#define O_N_UNIT_V(o,n)         (EV_LIST_VAL(EV_UNIT(OBSP_EVDATA(o)),n))
#define O_N_UNIT_T(o,n)         (EV_LIST_TIME(EV_UNIT(OBSP_EVDATA(o)),n))

#define O_N_ABORT_V(o,n)        (EV_LIST_VAL(EV_ABORT(OBSP_EVDATA(o)),n))
#define O_N_ABORT_T(o,n)        (EV_LIST_TIME(EV_ABORT(OBSP_EVDATA(o)),n))
#define O_N_WRONG_V(o,n)        (EV_LIST_VAL(EV_WRONG(OBSP_EVDATA(o)),n))
#define O_N_WRONG_T(o,n)        (EV_LIST_TIME(EV_WRONG(OBSP_EVDATA(o)),n))
#define O_N_PUNISH_V(o,n)       (EV_LIST_VAL(EV_PUNISH(OBSP_EVDATA(o)),n))
#define O_N_PUNISH_T(o,n)       (EV_LIST_TIME(EV_PUNISH(OBSP_EVDATA(o)),n))
#define O_N_SACCADE_V(o,n)      (EV_LIST_VAL(EV_SACCADE(OBSP_EVDATA(o)),n))
#define O_N_SACCADE_T(o,n)      (EV_LIST_TIME(EV_SACCADE(OBSP_EVDATA(o)),n))
#define O_N_BLANKING_V(o,n)     (EV_LIST_VAL(EV_BLANKING(OBSP_EVDATA(o)),n))
#define O_N_BLANKING_T(o,n)     (EV_LIST_TIME(EV_BLANKING(OBSP_EVDATA(o)),n))

/*
 * For the events with 4 params, times go by one, but vals skip by 5's
 *   (1 evdata + 4 params)
 */
#define O_N_INFO_T(o,n)         (EV_LIST_TIME(EV_INFO(OBSP_EVDATA(o)),n))
#define O_N_INFO_V(o,n)         (&EV_LIST_VAL(EV_INFO(OBSP_EVDATA(o)),n*5))


#define O_N_NSPIKES(o,n)         SP_CH_NSPTIMES(SP_CHANNEL(OBSP_SPDATA(o),n))
#define O_N_SPIKES(o,n)          SP_CH_SPTIMES(SP_CHANNEL(OBSP_SPDATA(o),n))
#define O_N_SPIKE(o,n,i)         SP_CH_SPTIMES(SP_CHANNEL(OBSP_SPDATA(o),n))[i]

/***********************************************************************
 *
 *   Structure: DF_INFO
 *   Refers to: None
 *   Found in:  DATA_FILE
 *   Purpose:   Contains info pertaining to whole file
 *
 ***********************************************************************/

enum DF_INFO_TAG {
  D_FILENAME_TAG, D_TIME_TAG, D_FILENUM_TAG, D_COMMENT_TAG, D_EXP_TAG,
  D_TMODE_TAG, D_EMCOLLECT_TAG, D_SPCOLLECT_TAG, D_NSTIMTYPES_TAG,
  D_AUXFILES_TAG
};

typedef struct {
  char *filename;		/* name of this file                   */
  int time;			/* seconds since Jan. 1, 1970, 00:00   */
  int nauxfiles;		/* number of associated filenames      */
  char **auxfiles;		/* array of filenames                  */
  int filenum;			/* number of this data file for exp.   */
  char *comment;		/* comment describing datafile         */
  int experiment;		/* experiment id code                  */
  int testmode;			/* test mode for this experiment       */
  int nstimtypes;		/* number of different stimuli in exp  */
  char emcollect;		/* were eye movements collected?       */
  char spcollect;		/* were spikes collected?              */
} DF_INFO;

#define DF_FILENAME(d)         ((d)->filename)
#define DF_NAUXFILES(d)        ((d)->nauxfiles)
#define DF_AUXFILES(d)         ((d)->auxfiles)
#define DF_AUXFILE(d,i)        ((d)->auxfiles[i])
#define DF_TIME(d)             ((d)->time)
#define DF_FILENUM(d)          ((d)->filenum)
#define DF_COMMENT(d)          ((d)->comment)
#define DF_EXPERIMENT(d)       ((d)->experiment)
#define DF_TESTMODE(d)         ((d)->testmode)
#define DF_NSTIMTYPES(d)       ((d)->nstimtypes)
#define DF_EMCOLLECT(d)        ((d)->emcollect)
#define DF_SPCOLLECT(d)        ((d)->spcollect)



/***********************************************************************
 *
 *   Structure: CELL_INFO
 *   Refers to: None
 *   Found in:  DATA_FILE
 *   Purpose:   Contains info pertaining to specific cell
 *
 ***********************************************************************/

enum CELL_INFO_TAG {
  C_NUM_TAG, C_DISCRIM_TAG, C_EV_TAG, C_XY_TAG, C_RFCENTER_TAG, C_DEPTH_TAG,
  C_TL_TAG, C_BL_TAG, C_BR_TAG, C_TR_TAG };

typedef struct {
  int monkey_id;                      /* monkey tattoo number (e.g. RUR-1) */
  int chamber_id;                     /* integer specifier for chamber     */
  int project_id;                     /* project identification            */
  int exper_id;                       /* specific experimental info        */
  int date_time;                     /* date/time cell was isolated       */
  int cell_number;                    /* ID number of cell                 */ 
  float discriminability;             /* isolation quality 0.0 - 1.0       */
  float evcoords[2];                  /* evarts coordinates of cell        */ 
  float xycoords[2];                  /* xy coordinates of cell            */
  float depth;                        /* depth of cell                     */
  char *description;                  /* descriptive notes about cell      */

/* To be read in from RF file */

  float  rfcenter[2];                  /* azimuth and elevation of rf       */
  float  rfdepth;                      /* preferred depth of rf             */
  float  rfquad[8];                    /* rectangular specifications of rf  */
  float  rfcontrast;                   /* preferred contrast of rf          */
  float  rforientation;                /* preferred orientation of rf       */
  int    rfcolor;                      /* preferred color of rf             */
  float  rfsigma;                      /* preferred stimulus size of rf     */
  float  rfspatfreq;                   /* preferred sf of rf                */
  int    rfmask;                       /* preferred mask of rf              */
} CELL_INFO;

#define CI_MONKEY_ID(c)        ((c)->monkeyID)
#define CI_CHAMBER_ID(c)       ((c)->chamberID)
#define CI_PROJECT_ID(c)       ((c)->project_id)
#define CI_EXPER_ID(c)         ((c)->exper_id)
#define CI_DATE_TIME(c)        ((c)->date_time)

#define CI_NUMBER(c)           ((c)->cell_number)
#define CI_DISCRIM(c)          ((c)->discriminability)
#define CI_EVCOORDS(c)         ((c)->evcoords)
#define CI_EVCOORDS_PHI(c)     ((c)->evcoords[0])
#define CI_EVCOORDS_THETA(c)   ((c)->evcoords[1])
#define CI_XYCOORDS(c)         ((c)->xycoords)
#define CI_XYCOORDS_X(c)       ((c)->xycoords[0])
#define CI_XYCOORDS_Y(c)       ((c)->xycoords[1])
#define CI_DEPTH(c)            ((c)->depth) 
#define CI_DESC(c)             ((c)->description)

#define CI_RF_CENTER(c)        ((c)->rfcenter)
#define CI_RF_ELEVATION(c)     ((c)->rfcenter[0])
#define CI_RF_AZIMUTH(c)       ((c)->rfcenter[1])
#define CI_RF_DEPTH(c)         ((c)->rfdepth)
#define CI_RF_ORI(c)           ((c)->rforientation)
#define CI_RF_COLOR(c)         ((c)->rfcolor)
#define CI_RF_CONTRAST(c)      ((c)->rfcontrast)
#define CI_RF_SIGMA(c)         ((c)->rfsigma)
#define CI_RF_SF(c)            ((c)->rfspatfreq)
#define CI_RF_MASK(c)          ((c)->rfmask)

#define CI_RF_QUAD_UL_X(c)     ((c)->rfquad[0])
#define CI_RF_QUAD_UL_Y(c)     ((c)->rfquad[1])
#define CI_RF_QUAD_UR_X(c)     ((c)->rfquad[2])
#define CI_RF_QUAD_UR_Y(c)     ((c)->rfquad[3])
#define CI_RF_QUAD_LL_X(c)     ((c)->rfquad[4])
#define CI_RF_QUAD_LL_Y(c)     ((c)->rfquad[5])
#define CI_RF_QUAD_LR_X(c)     ((c)->rfquad[6])
#define CI_RF_QUAD_LR_Y(c)     ((c)->rfquad[7])

#define CI_RF_QUAD_UL(c)       (&(c)->rfquad[0])
#define CI_RF_QUAD_UR(c)       (&(c)->rfquad[2])
#define CI_RF_QUAD_LR(c)       (&(c)->rfquad[4])
#define CI_RF_QUAD_LL(c)       (&(c)->rfquad[6])


typedef struct {
  int increment;		/* how much to reallocate by           */
  int max;			/* maximum slots currently av.         */
  int n;			/* number of slots filled              */
  CELL_INFO **vals;		/* pointer to cellinfo pointers        */
} DYN_CELL_LIST;

#define DCL_INCREMENT(dcl)      ((dcl)->increment)
#define DCL_MAX(dcl)            ((dcl)->max) 
#define DCL_VALS(dcl)           ((dcl)->vals)
#define DCL_VAL(dcl,i)          ((dcl)->vals[i])
#define DCL_N(dcl)              ((dcl)->n)





/***********************************************************************
 *
 *   Structure: DATA_FILE
 *   Refers to: DF_INFO, OBS_P, CELL_INFO
 *   Found in:  None
 *   Purpose:   The structurally organized representation of a datafile
 *
 ***********************************************************************/

enum DF_TAG {
  D_DFINFO_TAG, D_NOBSP_TAG, D_OBSP_TAG, D_NCINFO_TAG, D_CINFO_TAG
};

typedef struct {
  DF_INFO dfinfo;
  int     nobsp;
  OBS_P   *obsps;
  int     ncinfo;
  CELL_INFO *cinfos;
} DATA_FILE;

#define DF_DFINFO(d)     (&(d)->dfinfo)
#define DF_NOBSP(d)      ((d)->nobsp)
#define DF_OBSPS(d)      ((d)->obsps)
#define DF_OBSP(d,i)     (&DF_OBSPS(d)[i])
#define DF_NCINFO(d)     ((d)->ncinfo)
#define DF_CINFOS(d)     ((d)->cinfos)
#define DF_CINFO(d,i)    (&DF_CINFOS(d)[i])

/***********************************************************************
 *
 *   Structure: DYN_LIST
 *   Refers to: None
 *   Found in:  None
 *   Purpose:   Used as a means for managing dynamically growing,
 *              shrinking lists of data.
 *
 ***********************************************************************/

#define DYN_LIST_NAME_SIZE 64
typedef struct {
  char name[DYN_LIST_NAME_SIZE];/* buffer to hold name of list*/
  int datatype;			/* kind of data store in vals */
  int increment;		/* how much to reallocate by  */
  int max;			/* maximum slots currently av.*/
  int n;			/* number of slots filled     */
  int flags;			/* info about the dynlist     */
  void *vals;			/* pointer to actual data     */
} DYN_LIST;

#define DYN_LIST_NAME(d)      ((d)->name)
#define DYN_LIST_DATATYPE(d)  ((d)->datatype)
#define DYN_LIST_INCREMENT(d) ((d)->increment)
#define DYN_LIST_MAX(d)       ((d)->max)
#define DYN_LIST_N(d)         ((d)->n)
#define DYN_LIST_VALS(d)      ((d)->vals)
#define DYN_LIST_FLAGS(d)     ((d)->flags)

enum DL_FLAG {
  DL_SUBLIST = 0x01,
  DL_TCLOBJ = 0x02
};

/***********************************************************************
 *
 *   Structure: DYN_OLIST
 *   Refers to: None
 *   Found in:  None
 *   Purpose:   Used as a means for managing dynamically growing,
 *              shrinking lists of observation periods.
 *
 ***********************************************************************/

typedef struct {
  int increment;		/* how much to reallocate by  */
  int max;			/* maximum slots currently av.*/
  int n;			/* number of slots filled     */
  OBS_P **vals;			/* pointer to obsp pointers   */
} DYN_OLIST;

#define DYN_OLIST_INCREMENT(d) ((d)->increment)
#define DYN_OLIST_MAX(d)       ((d)->max)
#define DYN_OLIST_N(d)         ((d)->n)
#define DYN_OLIST_VALS(d)      ((d)->vals)

/***********************************************************************
 *
 *   Structure: DYN_GROUP
 *   Refers to: DYN_LIST
 *   Found in:  None
 *   Purpose:   Keep more than one dyn_list together (for em's, events)
 *
 ***********************************************************************/

#define DYN_GROUP_NAME_SIZE DYN_LIST_NAME_SIZE

typedef struct {
  char name[DYN_GROUP_NAME_SIZE];/* name of group              */
  int increment;		/* how much to reallocate by  */
  int max;			/* maximum slots currently av.*/
  int nlists;
  DYN_LIST **lists;		/* pointer to allocated lists */
} DYN_GROUP;

#define DYN_GROUP_NAME(d)      ((d)->name)
#define DYN_GROUP_INCREMENT(d) ((d)->increment)
#define DYN_GROUP_MAX(d)       ((d)->max)
#define DYN_GROUP_N(d)         ((d)->nlists)
#define DYN_GROUP_NLISTS(d)    ((d)->nlists)
#define DYN_GROUP_LISTS(d)     ((d)->lists)
#define DYN_GROUP_LIST(d,i)    (DYN_GROUP_LISTS(d)[i])


/***********************************************************************
 *
 *   Structure: BUF_DATA
 *   Refers to: None
 *   Found in:  None
 *   Purpose:   Keep internal index for the df event buffer.  Used
 *              for in the conversion of a dfBuffer -> DATA_FILE
 *
 ***********************************************************************/

typedef struct {
  unsigned char *buffer;
  int size;
  int index;
} BUF_DATA;

#define BD_BUFFER(b)     ((b)->buffer)
#define BD_SIZE(b)       ((b)->size)
#define BD_INDEX(b)      ((b)->index)
#define BD_INCINDEX(b,n) (BD_INDEX(b)+=n)
#define BD_DATA(b)       (&(b)->buffer[BD_INDEX(b)])
#define BD_GETC(b)       ((b)->buffer[BD_INDEX(b)++])
#define BD_EOF(b)        ((b)->index >= (b)->size)

/***********************************************************************
 *
 *                      DATA_FILE Function Prototypes
 *
 ***********************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

void dfInitBuffer(void);	              /* init a datafile buf   */
void dfResetBuffer(void);                     /* reset and initialize  */
void dfCloseBuffer(void);	              /* free mem assoc. w/buf */
void dfWriteBuffer(char *filename, char format);
void dfLoadStructure(DATA_FILE *df);

int  dfReadDataFile(char *, DATA_FILE *);
void dfLoadDataFile(DATA_FILE *df);
void dfDumpDataFile(DATA_FILE *df);

void dfRecordMagicNumber(void);

void dfRecordFlag(unsigned char);
void dfRecordChar(unsigned char, unsigned char);
void dfRecordLong(unsigned char, int);
void dfRecordShort(unsigned char, short);
void dfRecordFloat(unsigned char, float);

void dfRecordString(unsigned char, char *);
void dfRecordStringArray(unsigned char, int, char **);
void dfRecordLongArray(unsigned char, int, int *);
void dfRecordShortArray(unsigned char, int, short *);
void dfRecordFloatArray(unsigned char, int, float *);

void dfBeginStruct(unsigned char tag);
void dfEndStruct(void);

void dfPushStruct(int newstruct, char *);
int  dfPopStruct(void);
void dfFreeStructStack(void);
int  dfGetCurrentStruct(void);
char *dfGetCurrentStructName(void);
char *dfGetTagName(int type);
int  dfGetDataType(int type);
int  dfGetStructureType(int type);

void dfRecordDataFile(DATA_FILE *df);
void dfRecordDFInfo(unsigned char tag, DF_INFO *dfinfo);
void dfRecordObsPeriod(unsigned char tag, OBS_P *obsp);
void dfRecordObsInfo(unsigned char tag, OBS_INFO *oinfo);
void dfRecordEvData(unsigned char tag, EV_DATA *evdata);
void dfRecordEvList(unsigned char tag, EV_LIST *evlist);
void dfRecordEmData(unsigned char tag, EM_DATA *emdata);
void dfRecordSpData(unsigned char tag, SP_DATA *spdata);
void dfRecordSpChData(unsigned char tag, SP_CH_DATA *chdata);
void dfRecordCellInfo(unsigned char tag, CELL_INFO *cinfo);

void dfuFreeDataFile(DATA_FILE *df);
void dfuFreeObsPeriod(OBS_P *obsp);
void dfuFreeObsInfo(OBS_INFO *obsinfo);
void dfuFreeEvData(EV_DATA *evdata);
void dfuFreeEvList(EV_LIST *evlist);
void dfuFreeSpData(SP_DATA *spdata);
void dfuFreeSpChData(SP_CH_DATA *spchdata);
void dfuFreeEmData(EM_DATA *emdata);
void dfuFreeCellInfo(CELL_INFO *cinfo);
void dfuFreeDFInfo(DF_INFO *dfinfo);


DATA_FILE *dfuCreateDataFile(void);
int dfuCreateObsPeriods(DATA_FILE *df, int n);
OBS_P *dfuCreateObsPeriod(void);
int dfuCreateCellInfos(DATA_FILE *df, int n);
int dfuCreateSpikeChannels(SP_DATA *spdata, int n);
void dfuSetEmFixPos(EM_DATA *emdata, int x, int y);
void dfuSetEmWindow(EM_DATA *emdata, int v0, int v1, int v2, int v3);

void dfuSetSpChSource(SP_DATA *spdata, int channel, char source);
void dfuSetSpChCellnum(SP_DATA *spdata, int channel, int cellnum);

DYN_LIST *dfuCreateDynList(int type, int increment);
DYN_GROUP *dfuCreateDynGroup(int nlists);
DYN_LIST *dfuCreateDynListWithVals(int datatype, int n, void *vals);

DYN_LIST *dfuCreateNamedDynList(char *name, int type, int increment);
DYN_GROUP *dfuCreateNamedDynGroup(char *name, int nlists);
DYN_LIST *dfuCreateNamedDynListWithVals(char *name, int t, int n, void *vals);
DYN_GROUP *dfuCopyDynGroup(DYN_GROUP *dg, char *name);
int dfuAddDynGroupNewList(DYN_GROUP *, char *name, int type, int increment);
int dfuAddDynGroupExistingList(DYN_GROUP *dg, char *name, DYN_LIST *list);
int dfuCopyDynGroupExistingList(DYN_GROUP *dg, char *name, DYN_LIST *list);

DYN_LIST *dfuCopyDynList(DYN_LIST *old);

void dfuFreeDynList(DYN_LIST *);
void dfuResetDynList(DYN_LIST *);

DYN_OLIST *dfuCreateDynObsPeriods(void);
DYN_GROUP *dfuCreateDynEvData(void);
DYN_GROUP *dfuCreateDynSpData(int nchannels);
DYN_GROUP *dfuCreateDynEmData(void);

void dfuFreeDynOList(DYN_OLIST *);
void dfuFreeDynGroup(DYN_GROUP *);
void dfuResetDynGroup(DYN_GROUP *);

void dfuAddDynListLong(DYN_LIST *, int);
void dfuAddDynListShort(DYN_LIST *, short);
void dfuAddDynListFloat(DYN_LIST *, float);
void dfuAddDynListChar(DYN_LIST *, unsigned char);
void dfuAddDynListList(DYN_LIST *, DYN_LIST *);
void dfuAddDynListString(DYN_LIST *dynlist, char *string);

void dfuMoveDynListList(DYN_LIST *, DYN_LIST *);

void dfuPrependDynListLong(DYN_LIST *, int);
void dfuPrependDynListShort(DYN_LIST *, short);
void dfuPrependDynListFloat(DYN_LIST *, float);
void dfuPrependDynListChar(DYN_LIST *, unsigned char);
void dfuPrependDynListList(DYN_LIST *, DYN_LIST *);
void dfuPrependDynListString(DYN_LIST *dynlist, char *string);

int dfuInsertDynListLong(DYN_LIST *, int, int pos);
int dfuInsertDynListShort(DYN_LIST *, short, int pos);
int dfuInsertDynListFloat(DYN_LIST *, float, int pos);
int dfuInsertDynListChar(DYN_LIST *, unsigned char, int pos);
int dfuInsertDynListList(DYN_LIST *, DYN_LIST *, int pos);
int dfuInsertDynListString(DYN_LIST *dynlist, char *string, int pos);

void dfuAddObsPeriod(DYN_OLIST *dynolist, OBS_P *obsp);
void dfuAddEvData(DYN_GROUP *evgroup, int type, int val, int time);
void dfuAddEvData4Params(DYN_GROUP *evgroup, int type, int val, int time,
			 int p1, int p2, int p3, int p4);
void dfuAddEmData(DYN_GROUP *emgroup, short hsamp, short vsamp);
void dfuAddSpData(DYN_GROUP *spgroup, int channel, float time);

int  dfuSetObsPeriods(DATA_FILE *df, DYN_OLIST *dynolist);
int  dfuSetEvData(EV_DATA *evdata, DYN_GROUP *evlists);
int  dfuSetEvList(EV_LIST *, DYN_LIST *, DYN_LIST *);
int  dfuSetSpData(SP_DATA *spdata, DYN_GROUP *sptimes);
int  dfuSetEmData(EM_DATA *emdata, DYN_GROUP *emsamps);

void dfuFileToAscii(FILE *InFP, FILE *OutFP);
void dfuBufferToAscii(unsigned char *vbuf, int bufsize, FILE *OutFP);

int dfuFileToStruct(FILE *InFP, DATA_FILE *df);
int dfuFileToDataFile(FILE *InFp, DATA_FILE *df);
int dfuFileToDFInfo(FILE *InFp, DF_INFO *dfinfo);
int dfuFileToObsPeriod(FILE *InFp, OBS_P *obsp);
int dfuFileToObsInfo(FILE *InFP, OBS_INFO *oinfo);
int dfuFileToEvData(FILE *InFP, EV_DATA *evdata);
int dfuFileToEvList(FILE *InFP, EV_LIST *evlist);
int dfuFileToEmData(FILE *InFP, EM_DATA *emdata);
int dfuFileToSpData(FILE *InFP, SP_DATA *spdata);
int dfuFileToSpChData(FILE *InFP, SP_CH_DATA *spchdata);
int dfuFileToCellInfo(FILE *InFP, CELL_INFO *cinfo);

int dfuBufferToStruct(unsigned char *vbuf, int bufsize, DATA_FILE *df);
int dfuBufferToDataFile(BUF_DATA *bdata, DATA_FILE *df);
int dfuBufferToDFInfo(BUF_DATA *bdata, DF_INFO *dfinfo);
int dfuBufferToObsPeriod(BUF_DATA *bdata, OBS_P *obsp);
int dfuBufferToObsInfo(BUF_DATA *bdata, OBS_INFO *oinfo);
int dfuBufferToEvData(BUF_DATA *bdata, EV_DATA *evdata);
int dfuBufferToEvList(BUF_DATA *bdata, EV_LIST *evlist);
int dfuBufferToEmData(BUF_DATA *bdata, EM_DATA *emdata);
int dfuBufferToSpData(BUF_DATA *bdata, SP_DATA *spdata);
int dfuBufferToSpChData(BUF_DATA *bdata, SP_CH_DATA *spchdata);
int dfuBufferToCellInfo(BUF_DATA *bdata, CELL_INFO *cinfo);

#ifdef __cplusplus
}
#endif

#endif
