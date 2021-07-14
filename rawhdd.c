/* rawhdd - HDD imaging program for DOS.
 * Program used to image HDD (using BIOS functions) under DOS.
 * Use with either two HDD drives or with a networked drive.
 * see: http://hawk.ro/stories/everex286/
 *
 * Copyright 2019 Mihai Gaitos, mihaig@hawk.ro
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include <stdio.h>
#include <io.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <conio.h>
#include <dos.h>
#include <stdlib.h>
#include <bios.h>

/* BIOS table */
typedef struct hddparam
{
	unsigned int	cyls;
	unsigned char	heads;
	unsigned int	rwcc;	/* reduced write current cylinder */
	unsigned int	wpcc;	/* write pre-comp cylinder */
	unsigned char	ECCl;	/* (maximum) ECC burst length? */
	unsigned char	ctrb;	/* control byte? */
} hddparam;

/* options */
typedef struct myopts
{
	int	tracks;
	int	heads;
	int	sectors;
	int	drive;
	/* following are set to 1 if cyls/heads/sectors/drive is set */
	int ts;
	int hs;
	int ss;
	int ds;
} myopts;
/* this structure gymnastic is needed because drive can be selected
 * from options before detection but geometry switches must optionally
 * override detected values. */

/* globals used everywhere */
unsigned int sectors=0;
unsigned int tracks=0;
unsigned int heads=0;
unsigned char drive;
unsigned int trackbytes;

int dfh=0;	/* destination file handler */
FILE *lf=NULL;	/* log file */

int c_break(void)
{
	printf("Aborting on Ctrl-Break\n");
	close(dfh);
	fprintf(lf,"Aborted by Ctrl-Break!\n");
	fclose(lf);
	return 0;
}

int hddinfo()
/* obtain HDD info via 2 methods:
 * 1: read BIOS table (address is pointed to by interrupt vector 0x41)
 * 2: call int 13h,8
 * usually there is a difference of 1 cylinder between these two values.
 * if the difference is larger, or there is a difference in head numbers,
 * display a warning */
{
	int rv=0;
	union REGS regs;
	int cdif;
	hddparam far *hdp;
	if(drive==0x80)
		hdp=(hddparam far *)getvect(0x41);
	else
		hdp=(hddparam far *)getvect(0x46);
	/* funny how a function pointer points to a data table */

	regs.h.ah=0x08;
	regs.h.dl=drive;
	/* int 13h,8 */
	int86(0x13,&regs,&regs);
	if(regs.h.ah!=0)
	{
		fprintf(stderr,"Error reading disk information!\n");
		return 1;
	}
	sectors=regs.h.cl&0x3f;
	tracks=1+((regs.h.cl&0xc0)<<2)|regs.h.ch;
	heads=1+regs.h.dh;
	cdif=hdp->cyls-tracks;
	if(cdif>1)
	{
		fprintf(stderr,"WARNING: BIOS table cyls: %u; INT 13h,8 cyls: %u\n",hdp->cyls,tracks);
		rv=1;
	}
	tracks=hdp->cyls;
	if(heads!=hdp->heads)
	{
		fprintf(stderr,"WARNING: BIOS table heads %u; INT 13h,8 heads: %u\n",hdp->heads,heads);
		heads=hdp->heads; /* since we use table for cylinders, why not for heads, too? */
		rv=1;
	}
	return rv;
}

/* try to copy whole track (it's faster) */
int copy_track(unsigned int head,unsigned int track,void *buf,int f)
{
	if(biosdisk(2,drive,head,track,1,sectors,buf)!=0)
		return 1;
	if(write(f,buf,trackbytes)!=trackbytes)
		return -1;
	printf("CH %d,%d OK\n",track,head);
	return 0;
}

/* try to copy track sector-by-sector */
int copy_sects(unsigned int head,unsigned int track,void *buf,int f, FILE *lf)
{
	int i;
	int retr;
	int res;
	for(i=1;i<=sectors;i++)
	{
		if(biosdisk(2,drive,head,track,i,1,buf)!=0)
		{
			/* upon error retry up to 10 times */
			res=1;retr=10;
			while(retr>0 && res!=0)
			{
				printf("*");	/* one * means one failed read */
				/* reset controller before retrying */
				biosdisk(0,drive,0,0,0,1,NULL);
				res=biosdisk(2,drive,head,track,i,1,buf);
				retr--;
			}
			/* if read didn't succeed after multiple retries,
			 * print and log error */
			if(res!=0)
			{
				printf("Error reading CHS %d,%d,%d\n",track,head,i);
				fprintf(lf,"ERR: %d,%d,%d\n",track,head,i);
			}
			else /* success after some retries */
			{
				fprintf(lf,"OK: %d,%d,%d\n",track,head,i);
				printf(".");
			}
		}
		else	/* sector was read without retries */
		{
			fprintf(lf,"OK: %d,%d,%d\n",track,head,i);
			printf(".");
		}
		/* write no matter what (keep output in sync with disk position) */
		if(write(f,buf,512)!=512)
			return -1;	/* a write error probably means disk full, log will fail as well */
	}
	return 0;
}

void print_usage()
{
	printf("Usage: rawhdd [-d=drive] [-c=cylinders] [-h=heads] [-s=sectors] <dst_file>\n");
	printf("Will copy raw HDD \"image\" to dst_file.\nIf dst_file exists, it will be overwritten.\n");
	printf("The file rawhdd.log will be created (or appended to) and will log operations.\n");
	printf("Drive numbers are 0 based, i.e. first hard drive is numbered 0.\n");
}

int setopt(char *arg, myopts *opt)
{
	if(arg[0]==0) return -1;
	if(arg[0]!='-') return 1; /* destination file, hopefully */
	if(strlen(arg)<4) return -1; /* all switches are of the form "-x=n" */
	if(arg[2]!='=') return -1;
	switch(arg[1])
	{
		case 'c':
			opt->tracks=atoi(arg+3);
			opt->ts=1;
			return 0;
		case 'h':
			opt->heads=atoi(arg+3);
			opt->hs=1;
			return 0;
		case 's':
			opt->sectors=atoi(arg+3);
			opt->ss=1;
			return 0;
		case 'd':
			opt->drive=0x80+atoi(arg+3);
			opt->ds=1;
			return 0;
		default:
			return -1;
	}
}

int main(int argc,char *argv[])
{
	time_t t;
	struct tm *tms;
	struct myopts opts;
	int i, res;
	char *fn=NULL;
	char *buf;
	unsigned int track;
	unsigned int head;
	int rhi;

	/* "quick&dirty" options */
	memset(&opts,0,sizeof(opts));
	drive=0x80;	/* default */
	for(i=1;i<argc;i++)
	{
		res=setopt(argv[i],&opts);
		if(res<0)
		{
			print_usage();
			exit(1);
		}
		if(res==1)
		{
			if(fn==NULL)
				fn=argv[i];
			else /* more than one filename? */
			{
				print_usage();
				exit(1);
			}
		}
	}
	/* sanity check */
	if(fn==NULL)
	{
		print_usage();
		exit(1);
	}

	if(opts.ds)
		drive=opts.drive;


	printf("HDD Imaging program. Checking HDD...\n");
	if((rhi=hddinfo())<0)
	{
		fprintf(stderr,"ERROR: Unable to read HDD information via INT 13h\n");
		exit(1);
	}

	if(opts.ss)
		sectors=opts.sectors;
	if(opts.hs)
		heads=opts.heads;
	if(opts.ts)
		tracks=opts.tracks;

	if(tracks==0 || heads==0 || sectors==0)	/* any of these == 0 means nothing to read */
	{
		printf("Can't continue without geometry information.\n");
		printf("CHS: %u,%u,%u\n",tracks,heads,sectors);
		exit(1);
	}
	trackbytes=512*sectors;
	buf=malloc(trackbytes); /* one track */
	if(buf==NULL)
	{
		printf("malloc failed\n");
		exit(1);
	}

	/* print info and offer chance to abort */
	if(opts.ts || opts.hs || opts.ss)
		printf("Using command line drive geometry\n");
	printf("Will read: %u cylinders, %u heads, %u sectors\n",tracks,heads,sectors);
	printf("Will write to: %s\n",fn);
	if(rhi)
		printf("Possible geometry mismatch (see warning above)\nProceed at your own risk!\n");
	printf("Press ENTER to continue or any other key to abort\n");
	if(getch()!=13)
	{
		free(buf);
		exit(2);
	}

	dfh=open(fn,O_CREAT|O_BINARY|O_TRUNC|O_WRONLY,S_IREAD|S_IWRITE);
	if(dfh<1)
	{
		perror("Error creating destination file.\n");
		goto fail;
	}

	/* log */
	lf=fopen("rawhdd.log","at");
	t = time(NULL);
	tms = localtime(&t);
	fprintf(lf,"\n%s copy started at %s\n",fn,asctime(tms));
	fprintf(lf,"Drive %u CHS: %u,%u,%u\n",drive-0x80,tracks,heads,sectors);

	/* catch Ctrl+break (to write it in log before exiting) */
	ctrlbrk(c_break);

	/* read each head from each track */
	for(track=0;track<tracks;track++) for(head=0;head<heads;head++)
	{
		res=copy_track(head,track,buf,dfh);
		if(res==0)		/* log */
			fprintf(lf,"OK: %d,%d,*\n",track,head);
		if(res>0)     /* read track failed */
		{
			if((res=copy_sects(head,track,buf,dfh,lf))<0)  /* try sector by sector */
			{                          /* negative result means write failed */
				close(dfh);
				printf("write failed\n");
				goto fail;
			}
		}
		else if(res<0)  /* write file failed */
		{
			printf("write failed\n");
			goto fail;
		}
	}
	printf("Done.\n");
	close(dfh);
	t = time(NULL);
	tms = localtime(&t);
	fprintf(lf,"%s copy finished at %s\n",fn,asctime(tms));
	fclose(lf);
	free(buf);
	return(0);
fail:
	free(buf);
	if(dfh) close(dfh);
	if(lf!=NULL) fclose(lf);
	return(1);
}
