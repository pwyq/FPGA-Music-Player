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

#include "altera_avalon_pio_regs.h"
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
uint64_t CURRENT_BYTE = 0;

////////////////////////////////////////////////////////////////
int double_speed = 0;	// stereo
int half_speed = 0;		// stereo
int normal_speed = 0;	// stereo
int normal_mono = 0;	// mono
void update_lcd();

/////////////////////////////////
static void put_rc(FRESULT rc);

////////////////////////////////////////////////////////////////
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

void init_disk(uint8_t code)
{
	// Initialize disk
	xprintf("rc=%d\n", (uint16_t) disk_initialize((uint8_t) 0));	// "di 0"
	// Initialize file system
	put_rc(f_mount((uint8_t) 0, &Fatfs[0]));						// "fi 0"
}

// Open the LCD display file
void init_display()
{
	disp = fopen("/dev/lcd_display", "w");
}

// Logic to increment track index
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
	switch(IORD(BUTTON_PIO_BASE, 0)) {
		case 0xe:	// 1110
			next_song();
			update_lcd();
			NEXT_SONG = 1;
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
			update_lcd();
			PREV_SONG = 1;
			if (MODE == PLAYING)
				MODE = PLAYING;
			else
				MODE = STOPPED;
			break;
	}

	// Debounce
	while(IORD(BUTTON_PIO_BASE, 0) != 0xf) usleep(10000);

	IOWR(BUTTON_PIO_BASE, 3, 0x0);
}

// Initialize the ISRs
void init_button_pio()
{
	alt_irq_register(BUTTON_PIO_IRQ, (void *)0, handle_button_interrupts);
	IOWR(BUTTON_PIO_BASE, 2, 0xF);
}

// Open a file by name
void open_file(char *filename, uint8_t mode)
{
	f_open(&File1, filename, mode);
}

// Return 1 if a filename is a wav file
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

// Plays the currently selected file
void play_file()
{
    int fifospace;
    int i;
    int p1;
    int buffer_size;
    int res;
    int step;
    unsigned int l_buf;
    unsigned int r_buf;
    alt_up_audio_dev * audio_dev;

    p1 = song_sizes[curr_index];

    audio_dev = alt_up_audio_open_dev ("/dev/Audio");		// re-open every time

    open_file(song_list[curr_index], 1);
    step = determine_mode();

    while (p1)
    {
    	if(NEXT_SONG) {
    	    p1 = song_sizes[curr_index];
    	    open_file(song_list[curr_index], 1);
    	    NEXT_SONG = 0;
    	}

    	if(PREV_SONG) {
    	    p1 = song_sizes[curr_index];
    	    open_file(song_list[curr_index], 1);
    	    PREV_SONG = 0;
    	}

    	if(MODE == PLAYING) {

			buffer_size = 256;
			fifospace = alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT) * 4;

			if(buffer_size > fifospace) buffer_size = fifospace;

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
				alt_up_audio_write_fifo(audio_dev, &(l_buf), 1, ALT_UP_AUDIO_LEFT);
				if (normal_mono)
					alt_up_audio_write_fifo(audio_dev, &(l_buf), 1, ALT_UP_AUDIO_RIGHT);
				else
					alt_up_audio_write_fifo(audio_dev, &(r_buf), 1, ALT_UP_AUDIO_RIGHT);
			}
			p1 -= buffer_size;
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
	if (MODE == 0)
		tmp = "PLAY";
	else if (MODE == 1)
		tmp = "PAUSE";
	else if (MODE == 2)
		tmp = "STOP";
	fprintf(disp, "#%d %s\n", curr_index+1, song_list[curr_index]);
	fprintf(disp, "%s\n", tmp);
}

int main()
{
	init_disk(0);
	init_display();
	init_button_pio();

	song_index();
	open_file(song_list[curr_index], 1);	// first track

	// loop forever
	while(1) {
		switch (MODE) {
		case 0: // play
			update_lcd();
			play_file();
			break;
		case 1: // pause
			update_lcd();
			while(MODE == 1);
			break;
		case 2: // stop
			update_lcd();
			while(MODE == 2);
			break;
		}
	}

	return 0;
}

////////////////////////
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
