/*=========================================================================*/
/*  Includes                                                               */
/*=========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <system.h>
#include <sys/alt_alarm.h>
#include "sys/alt_irq.h"
#include <io.h>

#include "fatfs.h"
#include "diskio.h"
#include "ff.h"
#include "monitor.h"
#include "uart.h"
#include "alt_types.h"

#include <altera_up_avalon_audio.h>
#include <altera_up_avalon_audio_and_video_config.h>

#include <altera_avalon_timer.h>
#include <altera_avalon_timer_regs.h>

/*=========================================================================*/
/*  Global Variables                                                       */
/*=========================================================================*/
FILINFO Finfo;
FATFS Fatfs[_VOLUMES];			/* File system object for each logical drive */
FIL File1;						/* File objects */
FILE *disp;
DIR Dir;						/* Directory object */
uint8_t Buff[8192] __attribute__ ((aligned(4)));	 /* Working buffer */

char song_list[20][20];
uint64_t song_sizes[20];
uint16_t song_count = 0;
uint16_t curr_index = 0;
enum MODE_LIST{PLAYING, PAUSED, STOPPED};
volatile uint8_t MODE = STOPPED;
volatile uint8_t NEXT_SONG = 0;
volatile uint8_t PREV_SONG = 0;

volatile uint8_t double_speed = 0;	// stereo
volatile uint8_t half_speed = 0;		// stereo
volatile uint8_t normal_speed = 0;	// stereo
volatile uint8_t normal_mono = 0;	// mono
volatile uint8_t debounce_flag = 0;

/*=========================================================================*/
/*  Signatures                                                             */
/*=========================================================================*/
int determine_mode(void);
void next_song();
void prev_song();
static void handle_button_interrupts(void* context, uint32_t id);
static void timer_ISR(void* context, alt_u32 id);
void init_timer();
void init_button_pio();
void open_file(char *filename);
int isWav(char *filename);
void play_file();
void update_lcd();
void song_index();
void update_lcd();
static void put_rc(FRESULT rc);

/*=========================================================================*/
/*  FUNCTIONS                                                              */
/*=========================================================================*/
int determine_mode(void) {
	int status = -1;
	// reset
	double_speed = 0;
	half_speed = 0;
	normal_speed = 0;
	normal_mono = 0;

	status = (IORD(SWITCH_PIO_BASE, 0) & 0x03);
	if (status == 0) {
		// sw1 sw0
		// 0	0
		normal_speed = 1;
		xputs("Using Normal Speed-Stereo.\n");
	}
	else if (status == 1) {
		// 0 1
		half_speed = 1;
		xputs("Using Half Speed-Stereo.\n");
		return 2;
	}
	else if (status == 2) {
		// 1 0
		double_speed = 1;
		xputs("Using Double Speed-Stereo.\n");
		return 8;
	}
	else if (status == 3) {
		// 1 1
		normal_mono = 1;
		xputs("Using Normal Speed-Mono.\n");
	}
	return 4;
}

void next_song()
{
	curr_index = (curr_index + 1) % song_count;
}

void prev_song()
{
	curr_index--;
	if(curr_index < 0 || curr_index >=65535)
		curr_index = song_count-1;
}

// Button interrupt handler
static void handle_button_interrupts(void* context, uint32_t id)
{
	if (debounce_flag) {
		int tmp;
		tmp = determine_mode();
		switch(IORD(BUTTON_PIO_BASE, 0)) {
			case 0xe:	// 1110
				next_song();
				NEXT_SONG = 1;
				if (MODE == PLAYING)
					MODE = PLAYING;
				else
					MODE = STOPPED;
				update_lcd();
				break;
			case 0xd:	// 1101
				if(MODE == PAUSED || MODE == STOPPED)
					MODE = PLAYING;
				else
					MODE = PAUSED;
				update_lcd();
				break;
			case 0xb:	// 1011
				MODE = STOPPED;
				break;
			case 0x7:	// 0111
				prev_song();
				PREV_SONG = 1;
				if (MODE == PLAYING)
					MODE = PLAYING;
				else
					MODE = STOPPED;
				update_lcd();
				break;
		}
		debounce_flag = 0;
	}

	IOWR(BUTTON_PIO_BASE, 3, 0x0);
}

// Initialize the ISRs
void init_button_pio()
{
	alt_irq_register(BUTTON_PIO_IRQ, (void *)0, handle_button_interrupts);
	IOWR(BUTTON_PIO_BASE, 2, 0xF);
}

static void timer_ISR(void* context, alt_u32 id) {
	IOWR(SYSTEM_TIMER_BASE, 0, 0x0);	// clear TO
	debounce_flag = 1;
//	xprintf("end timer isr.\n");
}

void init_timer() {
	// register timer
	alt_irq_register(SYSTEM_TIMER_IRQ, (void*)0, timer_ISR);

	// clear IRQ status
	IOWR_ALTERA_AVALON_TIMER_STATUS(SYSTEM_TIMER_BASE, 0x0);

	// set period
	IOWR_ALTERA_AVALON_TIMER_PERIODL(SYSTEM_TIMER_BASE, 0xFF);
	IOWR_ALTERA_AVALON_TIMER_PERIODH(SYSTEM_TIMER_BASE, 0xFF);

	//start timer
	IOWR_ALTERA_AVALON_TIMER_CONTROL(SYSTEM_TIMER_BASE, ALTERA_AVALON_TIMER_CONTROL_START_MSK|
			ALTERA_AVALON_TIMER_CONTROL_ITO_MSK|
			ALTERA_AVALON_TIMER_CONTROL_CONT_MSK);	// turn on START, CONT, ITO
	// turn on ITO so that timer-core generates IRQ
}

void open_file(char *filename)
{
	f_open(&File1, filename, (uint8_t)1);	// mode is always 1
}

int isWav(char *filename)
{
	int len = strlen(filename);
	int i;
	if (len <= 4) {
		return 0;
	}
	char *last_four = &filename[len-4];
	// convert to lower case
	for (i = 0; i < 4; i++) {
		if (last_four[i] >=65 && last_four[i] <=92) {
			last_four[i] += 32;
		}
	}
	if (strcmp(last_four, ".wav") == 0) {
		return 1;
	}
	return 0;
}

void play_file()
{
    int i;
    int song_left;
    int buffer_size;
    int res;
    int step;
    unsigned int l_buf;
    unsigned int r_buf;
    alt_up_audio_dev * audio_dev;

    song_left = song_sizes[curr_index];

    audio_dev = alt_up_audio_open_dev ("/dev/Audio");		// re-open every time

    open_file(song_list[curr_index]);
    step = determine_mode();

    while (song_left)
    {
    	if(NEXT_SONG) {
    	    song_left = song_sizes[curr_index];
    	    open_file(song_list[curr_index]);
    	    NEXT_SONG = 0;
    	}

    	if(PREV_SONG) {
    	    song_left = song_sizes[curr_index];
    	    open_file(song_list[curr_index]);
    	    PREV_SONG = 0;
    	}

    	if(MODE == PLAYING) {
			buffer_size = 1024;
			res = f_read(&File1, Buff, buffer_size, &buffer_size);

			if(res != FR_OK) {
				put_rc(res);
				break;
			}

			for(i = 0; i < buffer_size; i += step)
			{
            	// take the first two bytes
            	l_buf = 0;			// clear
            	l_buf = l_buf | Buff[i+1];
            	l_buf = l_buf << 8;
            	l_buf = l_buf | Buff[i+0];

            	// take the next two bytes
            	r_buf = 0;
            	r_buf = r_buf | Buff[i+3];
            	r_buf = r_buf << 8;
            	r_buf = r_buf | Buff[i+2];
            	while (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT) < 1);	// while there is no fifospace, we wait
				alt_up_audio_write_fifo(audio_dev, &(l_buf), 1, ALT_UP_AUDIO_LEFT);
				if (normal_mono)
					alt_up_audio_write_fifo(audio_dev, &(l_buf), 1, ALT_UP_AUDIO_RIGHT);
				else
					alt_up_audio_write_fifo(audio_dev, &(r_buf), 1, ALT_UP_AUDIO_RIGHT);
			}
			if (song_left >= buffer_size)
				song_left -= buffer_size;
			else
				song_left = 0;
    	} else if(MODE == STOPPED) {
    		break;
    	}
    }
    // stay in current track
    if(MODE == PLAYING) {
    	MODE = STOPPED;
    }
}

void song_index()
{
	int res;
	int i = 0;

	res = f_opendir(&Dir, NULL);
	if (res) {
		put_rc(res);
		return;
	}
	for(;;) {
		res = f_readdir(&Dir, &Finfo);
		if ((res != FR_OK) || !Finfo.fname[0]) break;

		if(isWav(Finfo.fname)) {
			strcpy(song_list[i], Finfo.fname);
			song_sizes[i] = Finfo.fsize;
			i++;
		}
	}
	song_count = i;
}

void update_lcd()
{
	char *tmp;
	if (MODE == 0) {
		if (double_speed) {
			tmp = "PLAY-DBL SPD";
		}
		else if (half_speed) {
			tmp = "PLAY-HALF SPD";
		}
		else if (normal_speed) {
			tmp = "PLAY-NORM SPD";
		}
		else if (normal_mono) {
			tmp = "PLAY-MONO-L";
		}
	}
	else if (MODE == 1)
		tmp = "PAUSED";
	else if (MODE == 2)
		tmp = "STOPPED";
	fprintf(disp, "#%d %s\n", curr_index+1, song_list[curr_index]);
	fprintf(disp, "%s\n", tmp);
}

/*=========================================================================*/
/*  MAIN                                                                   */
/*=========================================================================*/
int main()
{
	// Initialize disk
	xprintf("rc=%d\n", (uint16_t) disk_initialize((uint8_t) 0));	// "di 0"
	// Initialize file system
	put_rc(f_mount((uint8_t) 0, &Fatfs[0]));						// "fi 0"
	// Init LCD
	disp = fopen("/dev/lcd_display", "w");
	init_button_pio();
	init_timer();

	song_index();						// get all song index
	open_file(song_list[curr_index]);	// first track

	// loop forever
	while(1) {
		switch (MODE) {
		case PLAYING:
			update_lcd();
			play_file();
			break;
		case PAUSED:
			update_lcd();
			while(MODE == PAUSED);
			break;
		case STOPPED:
			update_lcd();
			while(MODE == STOPPED);
			break;
		}
	}
	return 0;
}

/*=========================================================================*/
/*  HELPER FUNCTIONS                                                       */
/*=========================================================================*/
static void put_rc(FRESULT rc)
{
    const char *str =
        "OK\0" "DISK_ERR\0" "INT_ERR\0" "NOT_READY\0" "NO_FILE\0" "NO_PATH\0"
        "INVALID_NAME\0" "DENIED\0" "EXIST\0" "INVALID_OBJECT\0" "WRITE_PROTECTED\0"
        "INVALID_DRIVE\0" "NOT_ENABLED\0" "NO_FILE_SYSTEM\0" "MKFS_ABORTED\0" "TIMEOUT\0"
        "LOCKED\0" "NOT_ENOUGH_CORE\0" "TOO_MANY_OPEN_FILES\0";
    FRESULT i;

    for (i = 0; i != rc && *str; i++) {
        while (*str++);
    }
    xprintf("rc=%u FR_%s\n", (uint32_t) rc, str);
}
