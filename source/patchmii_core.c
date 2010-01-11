/*  patchmii_core -- low-level functions to handle the downloading, patching
    and installation of updates on the Wii

    Copyright (C) 2008 bushing / hackmii.com
    Copyright (C) 2008 WiiGator
    Copyright (C) 2009 svenpeter
	Copyright (C) 2009 Hermes

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 2.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ogcsys.h>
#include <gccore.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <network.h>
#include <sys/errno.h>
#include <fat.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "patchmii_core.h"
#include "sha1.h"
#include "debug.h"
#include "http.h"
#include "tools.h"
#include "IOSPatcher.h"
#include "haxx_certs.h"
#include "nus.h"
#include "gecko.h"

#ifdef TEMP_IOS
#include "uninstall.h"
#endif

#define VERSION "0.1"
#define TEMP_DIR "/tmp/patchmii"

#define ALIGN(a,b) ((((a)+(b)-1)/(b))*(b))

int http_status = 0;
int tmd_dirty = 0, tik_dirty = 0, temp_ios_slot = 0;

// yeah, yeah, I know.
signed_blob *s_tmd = NULL, *s_tik = NULL, *s_certs = NULL;
static u8 tmdbuf[MAX_SIGNED_TMD_SIZE] ATTRIBUTE_ALIGN(0x20);
static u8 tikbuf[STD_SIGNED_TIK_SIZE] ATTRIBUTE_ALIGN(0x20);

char ascii(char s) {
    if (s < 0x20) return '.';
    if (s > 0x7E) return '.';
    return s;
}

void hexdump(FILE *fp, void *d, int len) 
{
    u8 *data;
    int i, off;
    data = (u8*)d;
    for (off=0; off<len; off += 16) 
	{
        fprintf(fp, "%08x  ",off);
        for (i=0; i<16; i++)
		{
            if ((i+off)>=len) fprintf(fp, "   ");
            else fprintf(fp, "%02x ",data[off+i]);
		}

        fprintf(fp, " ");
        for (i=0; i<16; i++)
		{
            if ((i+off)>=len) fprintf(fp," ");
            else fprintf(fp,"%c",ascii(data[off+i]));
		}
        fprintf(fp,"\n");
    }
}

char *things[] = {"people", "hopes", "fail", "bricks", "firmware", "bugs", "hacks"};

void printvers(void) {
    gprintf("IOS Version: %08x\n", *((u32*)0xC0003140));
}

void decrypt_buffer(u16 index, u8 *source, u8 *dest, u32 len) {
    static u8 iv[16];
    if (!source) 
	{
        gprintf("decrypt_buffer: invalid source paramater\n");
        ReturnToLoader();
    }
    if (!dest) 
	{
        gprintf("decrypt_buffer: invalid dest paramater\n");
        ReturnToLoader();
    }

    memset(iv, 0, 16);
    memcpy(iv, &index, 2);
    aes_decrypt(iv, source, dest, len);
}

static u8 encrypt_iv[16];
void set_encrypt_iv(u16 index) {
    memset(encrypt_iv, 0, 16);
    memcpy(encrypt_iv, &index, 2);
}

void encrypt_buffer(u8 *source, u8 *dest, u32 len) {
    aes_encrypt(encrypt_iv, source, dest, len);
}

int create_temp_dir(void) {
    int retval;
    // Try to delete the temp directory in case we're starting over
    ISFS_Delete(TEMP_DIR);
    retval = ISFS_CreateDir (TEMP_DIR, 0, 3, 1, 1);
    if (retval) gprintf("ISFS_CreateDir(/tmp/patchmii) returned %d\n", retval);
    return retval;
}

u32 save_nus_object (u16 index, u8 *buf, u32 size) {
    char filename[256];
    static u8 bounce_buf[1024] ATTRIBUTE_ALIGN(0x20);
    u32 i;

    int retval, fd;
    snprintf(filename, sizeof(filename), "/tmp/patchmii/%08x", index);

    retval = ISFS_CreateFile (filename, 0, 3, 1, 1);

    if (retval != ISFS_OK) {
        gprintf("ISFS_CreateFile(%s) returned %d\n", filename, retval);
        return retval;
    }

    fd = ISFS_Open (filename, ISFS_ACCESS_WRITE);

    if (fd < 0) {
        gprintf("ISFS_OpenFile(%s) returned %d\n", filename, fd);
        return retval;
    }

    for (i=0; i<size;) 
	{
        u32 numbytes = ((size-i) < 1024)?size-i:1024;
        //spinner();
        memcpy(bounce_buf, buf+i, numbytes);
        retval = ISFS_Write(fd, bounce_buf, numbytes);
        if (retval < 0) {
            gprintf("ISFS_Write(%d, %p, %d) returned %d at offset %d\n",
                         fd, bounce_buf, numbytes, retval, i);
            ISFS_Close(fd);
            return retval;
        }
        i += retval;
    }
    ISFS_Close(fd);
    return size;
}

s32 install_nus_object (tmd *p_tmd, u16 index) 
{
    char filename[256];
    static u8 bounce_buf1[1024] ATTRIBUTE_ALIGN(0x20);
    static u8 bounce_buf2[1024] ATTRIBUTE_ALIGN(0x20);
    u32 i;
    const tmd_content *p_cr = TMD_CONTENTS(p_tmd);
//  gprintf("install_nus_object(%p, %lu)\n", p_tmd, index);

    int retval, fd, cfd, ret;
    snprintf(filename, sizeof(filename), "/tmp/patchmii/%08x", p_cr[index].cid);

    //spinner();
    fd = ISFS_Open(filename, ISFS_ACCESS_READ);

    if (fd < 0) 
	{
        gprintf("ISFS_OpenFile(%s) returned %d\n", filename, fd);
        return fd;
    }
    set_encrypt_iv(index);
//  gprintf("ES_AddContentStart(%016llx, %x)\n", p_tmd->title_id, index);

    cfd = ES_AddContentStart(p_tmd->title_id, p_cr[index].cid);
    if (cfd < 0) 
	{
        printf(":\nES_AddContentStart(%016llx, %x) failed: %d\n", p_tmd->title_id, index, cfd);
        ES_AddTitleCancel();
        return -1;
    }
// gprintf("\b (cfd %d): ",cfd);
    for (i=0; i<p_cr[index].size;) {
        u32 numbytes = ((p_cr[index].size-i) < 1024)?p_cr[index].size-i:1024;
        //spinner();
        numbytes = ALIGN(numbytes, 32);
        retval = ISFS_Read(fd, bounce_buf1, numbytes);
        if (retval < 0) {
            gprintf("ISFS_Read(%d, %p, %d) returned %d at offset %d\n", fd, bounce_buf1, numbytes, retval, i);
            ES_AddContentFinish(cfd);
            ES_AddTitleCancel();
            ISFS_Close(fd);
            return retval;
        }

        encrypt_buffer(bounce_buf1, bounce_buf2, sizeof(bounce_buf1));
        ret = ES_AddContentData(cfd, bounce_buf2, retval);
        if (ret < 0) {
            gprintf("ES_AddContentData(%d, %p, %d) returned %d\n", cfd, bounce_buf2, retval, ret);
            ES_AddContentFinish(cfd);
            ES_AddTitleCancel();
            ISFS_Close(fd);
            return ret;
        }
        i += retval;
    }

// gprintf("\b  done! (0x%x bytes)\n",i);
    ret = ES_AddContentFinish(cfd);
    if (ret < 0) {
        printf("ES_AddContentFinish failed: %d\n",ret);
        ES_AddTitleCancel();
        ISFS_Close(fd);
        return -1;
    }
    //spinner();
    ISFS_Close(fd);

    return 0;
}

int get_title_key(signed_blob *s_tik, u8 *key) {
    static u8 iv[16] ATTRIBUTE_ALIGN(0x20);
    static u8 keyin[16] ATTRIBUTE_ALIGN(0x20);
    static u8 keyout[16] ATTRIBUTE_ALIGN(0x20);
    int retval;

    const tik *p_tik;
    p_tik = (tik*)SIGNATURE_PAYLOAD(s_tik);
    u8 *enc_key = (u8 *)&p_tik->cipher_title_key;
    memcpy(keyin, enc_key, sizeof keyin);
    memset(keyout, 0, sizeof keyout);
    memset(iv, 0, sizeof iv);
    memcpy(iv, &p_tik->titleid, sizeof p_tik->titleid);

    retval = ES_Decrypt(ES_KEY_COMMON, iv, keyin, sizeof keyin, keyout);
    if (retval) gprintf("ES_Decrypt returned %d\n", retval);
    memcpy(key, keyout, sizeof keyout);
    return retval;
}

int change_ticket_title_id(signed_blob *s_tik, u32 titleid1, u32 titleid2) {
    static u8 iv[16] ATTRIBUTE_ALIGN(0x20);
    static u8 keyin[16] ATTRIBUTE_ALIGN(0x20);
    static u8 keyout[16] ATTRIBUTE_ALIGN(0x20);
    int retval;

    tik *p_tik;
    p_tik = (tik*)SIGNATURE_PAYLOAD(s_tik);
    u8 *enc_key = (u8 *)&p_tik->cipher_title_key;
    memcpy(keyin, enc_key, sizeof keyin);
    memset(keyout, 0, sizeof keyout);
    memset(iv, 0, sizeof iv);
    memcpy(iv, &p_tik->titleid, sizeof p_tik->titleid);

    retval = ES_Decrypt(ES_KEY_COMMON, iv, keyin, sizeof keyin, keyout);
    p_tik->titleid = (u64)titleid1 << 32 | (u64)titleid2;
    memset(iv, 0, sizeof iv);
    memcpy(iv, &p_tik->titleid, sizeof p_tik->titleid);

    retval = ES_Encrypt(ES_KEY_COMMON, iv, keyout, sizeof keyout, keyin);
    if (retval) gprintf("ES_Decrypt returned %d\n", retval);
    memcpy(enc_key, keyin, sizeof keyin);
    tik_dirty = 1;

    return retval;
}

s32 get_title_version(u32 titleid1, u32 titleid2) {
    u32 tmdsize=0;
    s32 retval;
    static char tmd_buf[1024] ATTRIBUTE_ALIGN(32);
    signed_blob *stmd = (signed_blob *)&tmd_buf[0];
    int version;
    u64 titleid =  (u64)titleid1 << 32 | (u64)titleid2;
    retval = ES_GetStoredTMDSize(titleid, &tmdsize);
    if (retval < 0) {
        if (retval != -106) gprintf("ES_GetStoredTMDSize(%llx) = %x, retval=%d\n", titleid, tmdsize, retval);
        return retval;
    }

    retval = ES_GetStoredTMD(titleid, stmd, tmdsize);

    if (retval < 0) {
        gprintf("ES_GetStoredTMD returned %d\n", retval);
        return retval;
    }

    tmd *mytmd = (tmd*)SIGNATURE_PAYLOAD(stmd);
    version = mytmd->title_version;
    return version;
}

void change_tmd_version(signed_blob *s_tmd, u32 version) 
{
    tmd *p_tmd;
    p_tmd = (tmd*)SIGNATURE_PAYLOAD(s_tmd);
    p_tmd->title_version = version;
    tmd_dirty = 1;
}

void change_tmd_title_id(signed_blob *s_tmd, u32 titleid1, u32 titleid2) 
{
    tmd *p_tmd;
    u64 title_id = titleid1;
    title_id <<= 32;
    title_id |= titleid2;
    p_tmd = (tmd*)SIGNATURE_PAYLOAD(s_tmd);
    p_tmd->title_id = title_id;
    tmd_dirty = 1;
}


void display_tag(u8 *buf) 
{
    gprintf("Firmware version: %s      Builder: %s", buf, buf+0x30);
}

void display_ios_tags(u8 *buf, u32 size) {
    u32 i;
    char *ios_version_tag = "$IOSVersion:";

    if (size == 64) 
	{
        display_tag(buf);
        return;
    }

    for (i=0; i<(size-64); i++) 
	{
        if (!strncmp((char *)buf+i, ios_version_tag, 10)) 
		{
            char version_buf[128], *date;
            while (buf[i+strlen(ios_version_tag)] == ' ') i++; // skip spaces
            strlcpy(version_buf, (char *)buf + i + strlen(ios_version_tag), sizeof version_buf);
            date = version_buf;
            strsep(&date, "$");
            date = version_buf;
            strsep(&date, ":");
            gprintf("%s (%s)\n", version_buf, date);
            i += 64;
        }
    }
}

void print_tmd_summary(const tmd *p_tmd) 
{
    const tmd_content *p_cr;
    p_cr = TMD_CONTENTS(p_tmd);

    u32 size=0;

    u16 i=0;
    for (i=0;i<p_tmd->num_contents;i++) size += p_cr[i].size;

    gprintf("Title ID: %016llx\n",p_tmd->title_id);
	gprintf("Title Version: %u", p_tmd->title_version);
    gprintf("Number of parts: %d.  Total size: %uK\n", p_tmd->num_contents, (u32) (size / 1024));
}

void zero_sig(signed_blob *sig) 
{
    u8 *sig_ptr = (u8 *)sig;
    memset(sig_ptr + 4, 0, SIGNATURE_SIZE(sig)-4);
}

void brute_tmd(tmd *p_tmd) {
    u16 fill;
    for (fill=0; fill<65535; fill++) 
	{
        p_tmd->fill3=fill;
        sha1 hash;
        //gprintf("SHA1(%p, %x, %p)\n", p_tmd, TMD_SIZE(p_tmd), hash);
        SHA1((u8 *)p_tmd, TMD_SIZE(p_tmd), hash);;

        if (hash[0]==0) 
		{
            gprintf("setting fill3 to %04hx\n", fill);
            return;
        }
    }
    printf("Unable to fix tmd :(\n");
    ReturnToLoader();
}

void brute_tik(tik *p_tik) 
{
    u16 fill;
    for (fill=0; fill<65535; fill++) 
	{
        p_tik->padding=fill;
        sha1 hash;
        //    gprintf("SHA1(%p, %x, %p)\n", p_tmd, TMD_SIZE(p_tmd), hash);
        SHA1((u8 *)p_tik, sizeof(tik), hash);
        if (hash[0]==0) return;
    }
    printf("Unable to fix tik :(\n");
    ReturnToLoader();
}

static void forge_tmd(signed_blob *s_tmd) 
{
	gprintf("forging tmd sig\n");
    zero_sig(s_tmd);
    brute_tmd(SIGNATURE_PAYLOAD(s_tmd));
}

static void forge_tik(signed_blob *s_tik) 
{
	gprintf("forging tik sig\n");
    zero_sig(s_tik);
    brute_tik(SIGNATURE_PAYLOAD(s_tik));
}

#define BLOCK 0x1000

int install_ticket(const signed_blob *s_tik, const signed_blob *s_certs, u32 certs_len) 
{
    u32 ret;

	//  gprintf("Installing ticket...\n");
    ret = ES_AddTicket(s_tik, STD_SIGNED_TIK_SIZE, s_certs, certs_len, NULL, 0);
    if (ret < 0) 
	{
        gprintf("ES_AddTicket failed: %d\n",ret);
        return ret;
    }
    return 0;
}

s32 install(const signed_blob *s_tmd, const signed_blob *s_certs, u32 certs_len) 
{
    u32 ret, i;
    tmd *p_tmd = SIGNATURE_PAYLOAD(s_tmd);
//  gprintf("Adding title...\n");

    ret = ES_AddTitleStart(s_tmd, SIGNED_TMD_SIZE(s_tmd), s_certs, certs_len, NULL, 0);

    //spinner();
    if (ret < 0) 
	{
        gprintf("ES_AddTitleStart failed: %d\n",ret);
        ES_AddTitleCancel();
        return ret;
    }

    for (i=0; i<p_tmd->num_contents; i++) 
	{
		//gprintf("Adding content ID %08x", i);
        printf("\b%u....", i+1);
        ret = install_nus_object((tmd *)SIGNATURE_PAYLOAD(s_tmd), i);
        if (ret) return ret;
    }
    printf("\b!\n");

    ret = ES_AddTitleFinish();
    if (ret < 0) 
	{
        printf("ES_AddTitleFinish failed: %d\n",ret);
        ES_AddTitleCancel();
        return ret;
    }

//  printf("Installation complete!\n");
    return 0;
}

void patchmii_download(u32 titleid1, u32 titleid2, u16 *version, bool patch, bool patch2) 
{
    u8 *temp_tmdbuf = NULL, *temp_tikbuf = NULL;
    u32 tmdsize;
    u8 update_tmd;
    char tmdstring[20] = "tmd";
    int i, retval;
	//gprintf("\npatchmii_download() useSd = %d",useSd);

    if (ISFS_Initialize() || create_temp_dir()) 
	{
        perror("Failed to create temp dir: ");
        ReturnToLoader();
    }
    
    if (*version != 0) sprintf(tmdstring, "%s.%u", tmdstring, *version);

    printf("TMD...");
	SpinnerStart();
    retval = GetNusObject(titleid1, titleid2, version, tmdstring, &temp_tmdbuf, &tmdsize);
	SpinnerStop();

    if (retval < 0) 
	{
        gprintf("GetNusObject(tmd) returned %d, tmdsize = %u\n", retval, tmdsize);
        ReturnToLoader();
    }
    if (temp_tmdbuf == NULL) 
	{
        gprintf("Failed to allocate temp buffer for encrypted content, size was %u\n", tmdsize);
        ReturnToLoader();
    }
    memcpy(tmdbuf, temp_tmdbuf, MIN(tmdsize, sizeof(tmdbuf)));
    free(temp_tmdbuf);

    s_tmd = (signed_blob *)tmdbuf;
    if (!IS_VALID_SIGNATURE(s_tmd)) 
	{
        gprintf("Bad TMD signature!\n");
        ReturnToLoader();
    }
    printf("\b.Done\n");

    printf("Ticket...");
    u32 ticketsize;

	SpinnerStart();
    retval = GetNusObject(titleid1, titleid2, version, "cetk", &temp_tikbuf, &ticketsize);
	SpinnerStop();

    if (retval < 0) gprintf("GetNusObject(cetk) returned %d, ticketsize = %u\n", retval, ticketsize);
    memcpy(tikbuf, temp_tikbuf, MIN(ticketsize, sizeof(tikbuf)));

    s_tik = (signed_blob *)tikbuf;
    if (!IS_VALID_SIGNATURE(s_tik)) {
        gprintf("Bad tik signature!\n");
        ReturnToLoader();
    }

    free(temp_tikbuf);

    printf("\b.Done\n");

    s_certs = (signed_blob *)haxx_certs;
    if (!IS_VALID_SIGNATURE(s_certs)) 
	{
        printf("Bad cert signature!\n");
        ReturnToLoader();
    }

    u8 key[16];
    get_title_key(s_tik, key);
    aes_set_key(key);

    const tmd *p_tmd;
    tmd_content *p_cr;
    p_tmd = (tmd*)SIGNATURE_PAYLOAD(s_tmd);
    p_cr = TMD_CONTENTS((tmd*)p_tmd);

//	print_tmd_summary(p_tmd);

//	gprintf("\b ..games..\b");

    static char cidstr[32];

    for (i=0;i<p_tmd->num_contents;i++) 
	{
//		gprintf("Downloading part %d/%d (%lluK): ", i+1,
//					p_tmd->num_contents, p_cr[i].size / 1024);
        sprintf(cidstr, "%08x", p_cr[i].cid);

        u8 *content_buf, *decrypted_buf;
        u32 content_size;

        printf("\bContent %u/%u ID: %08x....", i+1, p_tmd->num_contents, p_cr[i].cid);
		SpinnerStart();
        retval = GetNusObject(titleid1, titleid2, version, cidstr, &content_buf, &content_size);
		SpinnerStop();
        if (retval < 0) 
		{
            gprintf("GetNusObject(%s) failed with error %d, content size = %u\n", cidstr, retval, content_size);
            ReturnToLoader();
        }

        if (content_buf == NULL) 
		{
            gprintf("error allocating content buffer, size was %u\n", content_size);
            ReturnToLoader();
        }

        if (content_size % 16) 
		{
            gprintf("ERROR: downloaded content[%d] size %u is not a multiple of 16\n", i, content_size);
            free(content_buf);
            ReturnToLoader();
        }

        if (content_size < p_cr[i].size) 
		{
            gprintf("ERROR: only downloaded %u / %llu bytes\n", content_size, p_cr[i].size);
            free(content_buf);
            ReturnToLoader();
        }

        decrypted_buf = malloc(content_size);
        if (!decrypted_buf) {
            gprintf("ERROR: failed to allocate decrypted_buf (%u bytes)\n", content_size);
            free(content_buf);
            ReturnToLoader();
        }

        decrypt_buffer(i, content_buf, decrypted_buf, content_size);

        printf("\b.Done\n");

        sha1 hash;
        SHA1(decrypted_buf, p_cr[i].size, hash);

        if (!memcmp(p_cr[i].hash, hash, sizeof hash)) 
		{
//			gprintf("\b\b hash OK. ");
//			display_ios_tags(decrypted_buf, content_size);

            update_tmd = 0;
            if (patch) 
			{
                if ((p_tmd->title_id >> 32) == 1 && ((u32)(p_tmd->title_id)) > 2) 
				{
                    if (patch_hash_check(decrypted_buf, content_size)) update_tmd = 1;
                }
            }

            if (patch2) 
			{
                if ((p_tmd->title_id >> 32) == 1 && ((u32)(p_tmd->title_id)) > 2) 
				{
                    if (patch_identify_check(decrypted_buf, content_size)) update_tmd = 1;
                }
            }

            if (update_tmd == 1) 
			{
				// gprintf("Updating TMD.\n");
                SHA1(decrypted_buf, p_cr[i].size, hash);
                memcpy(p_cr[i].hash, hash, sizeof hash);
                if (p_cr[i].type == 0x8001) p_cr[i].type = 1;
                tmd_dirty=1;
            }

            retval = (int) save_nus_object(p_cr[i].cid, decrypted_buf, content_size);
            if (retval < 0) 
			{
                printf("save_nus_object(%x) returned error %d\n", p_cr[i].cid, retval);
                ReturnToLoader();
            }
        } 
		else 
		{
            printf("hash BAD\n");
            ReturnToLoader();
        }

        free(decrypted_buf);
        free(content_buf);
    }
//	gprintf("\b ..keys..\b");

}

s32 patchmii_install(u32 in_title_h, u32 in_title_l, u16 in_version, u32 out_title_h, u32 out_title_l, u16 out_version, bool patch, bool patch2) 
{
	//gprintf("\npatchmii_install()");

	u16 version = in_version;

    if (version) gcprintf("Downloading Title %08x-%08x v%u.....\n", in_title_h, in_title_l, version);
    else gcprintf("Downloading Title %08x-%08x.....\n", in_title_h, in_title_l);

    patchmii_download(in_title_h, in_title_l, &version, patch,patch2);
    if (in_title_h != out_title_h || in_title_l != out_title_l ) 
	{
        change_ticket_title_id(s_tik, out_title_h, out_title_l);
        change_tmd_title_id(s_tmd, out_title_h, out_title_l);
        tmd_dirty = 1;
        tik_dirty = 1;
    }
    if (version != out_version) 
	{
        change_tmd_version(s_tmd, out_version);
        tmd_dirty = 1;
        tik_dirty = 1;
    }

    if (tmd_dirty) 
	{
        forge_tmd(s_tmd);
        tmd_dirty = 0;
    }

    if (tik_dirty) 
	{
        forge_tik(s_tik);
        tik_dirty = 0;
    }

    if (out_version) gcprintf("\bDownload complete. Installing to Title %08x-%08x v%u...\n", out_title_h, out_title_l, out_version);
    else gcprintf("\bDownload complete. Installing to Title %08x-%08x...\n", out_title_h, out_title_l);

    int retval = install_ticket(s_tik, s_certs, haxx_certs_size);
    if (retval) 
	{
        gprintf("install_ticket returned %d\n", retval);
        ReturnToLoader();
    }
	
	SpinnerStart();
    retval = install(s_tmd, s_certs, haxx_certs_size);
	SpinnerStop();
//	gprintf("\b..hacks..\b");

    if (retval) printf("install returned %d\n", retval);
    printf("\bInstallation complete!\n");
    return retval;
}


#ifdef TEMP_IOS
s32 find_empty_IOS_slot(void) {
    int i;
    for (i=255; i>64; i--) {
        if (get_title_version(1,i)==-106) break;
    }
    if (i>64) {
//		gprintf("Found empty IOS slot (IOS%d)\n", i);
        temp_ios_slot = i;
        return i;
    }
    gprintf("Couldn't find empty IOS slot :(\n");
    return -1;
}

s32 install_temporary_ios(u32 base_ios, u32 base_ver) {
    if (find_empty_IOS_slot()==-1) return -1;
    return patchmii_install(1, base_ios, base_ver, 1, temp_ios_slot, 31337, 1);
    /*patchmii_download(1, base_ios, base_ver);
    change_ticket_title_id(s_tik, 1, temp_ios_slot);
    change_tmd_title_id(s_tmd, 1, temp_ios_slot);
    change_tmd_version(s_tmd, 31337);
       forge_tmd(s_tmd);
       forge_tik(s_tik);

    //  	gprintf("Download complete. Installing:\n");

      int retval = install_ticket(s_tik, s_certs, haxx_certs_size);
      if (retval) {
    	gprintf("install_ticket returned %d\n", retval);
    	exit(1);
      }

      retval = install(s_tmd, s_certs, haxx_certs_size);
    //    gprintf("\b..hacks..\b");

    if (retval) printf("install returned %d\n", retval);
    return retval;*/
}

s32 load_temporary_ios(void) {
    ISFS_Deinitialize();
    net_deinit();
    return IOS_ReloadIOS(temp_ios_slot);
}

s32 cleanup_temporary_ios(void) {
    gprintf("Cleaning up temporary IOS version %d", temp_ios_slot);
    if (temp_ios_slot < 64) { // this should never happen
        printf("Not gonna do it, would't be prudent...\n");
        while (1);
    }

    /*	This code should work, but ends up getting an error -1017...
    	s32 vers = get_title_version(1, temp_ios_slot);
    	gprintf("Detected version %d of IOS%d\n", vers, temp_ios_slot);
    	if (vers != 31337) {
    		gprintf("Error: we didn't make this version of IOS\n");
    		return -1;
    	} */

    u64 title = (u64) 1 << 32 | (u64)temp_ios_slot;
    return Uninstall_FromTitle(title);
}
#endif
