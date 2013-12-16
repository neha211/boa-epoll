/*
 *  Boa, an http server
 *  Copyright (C) 1995 Paul Phillips <paulp@go2net.com>
 *  Some changes Copyright (C) 1996 Charles F. Randall <crandall@goldsys.com>
 *  Some changes Copyright (C) 1996 Larry Doolittle <ldoolitt@boa.org>
 *  Some changes Copyright (C) 1996-2002 Jon Nelson <jnelson@boa.org>
 *
 *  This file was added to boa (0.94.13)
 *  As a part of the patch of enhancing it
 *  with event based epoll approach
 *
 */

/* $Id: epoll.c, 2013/12/14 17:00:00 pesit Exp $*/

#include "boa.h"

/*
 * Name: KEVENT_ISSET
 *
 * Description: Macro to check if the event has occurred.
 * Meant only for local use.
 */

#define KEVENT_ISSET(cfd, ctype, kevents, count) {\
    int i=0;\
    return_val = 0;\
    for (i=0; i<=count; ++i) \
        if(kevents[i].ident == cfd && (kevents[i].flags && ctype)) {\
            return_val = 1; \
            break; \
        } \
}

static void fdset_update(void);
struct kevent event;
struct kevent kevents[MAX_EVENTS];
int return_val;
int nkev;
int kqfd;

void kqueue_loop(int server_s)
{
    if ((kqfd = kqueue()) < 0) {
        DIE("kqueue");
    }
    
	/* register server_s */
    EV_SET(&event, server_s, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kqfd, &event, 1, NULL, 0, NULL) == -1) {
        DIE("first_kevent");
    }
    
    while (1) {
        if (sighup_flag)
            sighup_run();
        if (sigchld_flag)
            sigchld_run();
        if (sigalrm_flag)
            sigalrm_run();
        
        if (sigterm_flag) {
            if (sigterm_flag == 1)
                sigterm_stage1_run(server_s);
            if (sigterm_flag == 2 && !request_ready && !request_block) {
                sigterm_stage2_run();
            }
        }
        
        if (request_block)
        /* move selected req's from request_block to request_ready */
            fdset_update();
        
        /* any blocked req's move from request_ready to request_block */
        process_requests(server_s);
        
        int time_out = (request_ready ? 0 : (ka_timeout ? ka_timeout : REQUEST_TIMEOUT));
        struct timespec t_sec;
        t_sec.tv_sec = time_out;
        
        nkev = kevent(kqfd, NULL, 0, kevents, MAX_EVENTS, (request_ready || request_block ? &t_sec : NULL));
        
        if (nkev < 0) {
            if (errno == EINTR)
                continue;   /* while(1) */
            else if (errno != EBADF) {
                DIE("kevent again");
            }
        }
            
        time(&current_time);
            
        KEVENT_ISSET(server_s, EVFILT_READ, kevents, nkev);
            
        if (return_val)      
            pending_requests = 1;
            
    }   
}

/*
 * Name: fdset_update
 *
 * Description: iterate through the blocked requests, checking whether
 * that file descriptor has been set by the event.  Update kevents to
 * reflect current status.
 */
   
static void fdset_update(void)
{
    request *current, *next;
    
    for(current = request_block;current;current = next) {
        time_t time_since = current_time - current->time_last;
        next = current->next;
        
        /* hmm, what if we are in "the middle" of a request and not
         * just waiting for a new one... perhaps check to see if anything
         * has been read via header position, etc... */
        if (current->kacount < ka_max && /* we *are* in a keepalive */
            (time_since >= ka_timeout) && /* ka timeout */
            !current->logline)  /* haven't read anything yet */
            current->status = DEAD; /* connection keepalive timed out */
        else if (time_since > REQUEST_TIMEOUT) {
            log_error_doc(current);
            fputs("connection timed out\n", stderr);
            current->status = DEAD;
        }
        
        if (current->buffer_end && current->status < DEAD) {
            KEVENT_ISSET(current->fd, EVFILT_WRITE, kevents, nkev);
            if (return_val)
                ready_request(current);
            else {
                KEV_CTL(current->fd, EVFILT_WRITE, EV_ADD);
            }
        } else {
            switch (current->status) {
                case WRITE:
                case PIPE_WRITE:
                    KEVENT_ISSET(current->fd, EVFILT_WRITE, kevents, nkev);
                    if (return_val)
                        ready_request(current);
                    else {
                        KEV_CTL(current->fd, EVFILT_WRITE, EV_ADD);
                    }
                    break;
                case BODY_WRITE:
                    KEVENT_ISSET(current->post_data_fd, EVFILT_WRITE, kevents, nkev);
                    if (return_val)
                        ready_request(current);
                    else {
                        KEV_CTL(current->post_data_fd, EVFILT_WRITE, EV_ADD);
                    }
                    break;
                case PIPE_READ:
                    KEVENT_ISSET(current->data_fd, EVFILT_READ, kevents, nkev);
                    if (return_val)
                        ready_request(current);
                    else {
                        KEV_CTL(current->data_fd, EVFILT_READ, EV_ADD);
                    }
                    break;
                case DONE:
                    KEVENT_ISSET(current->fd, EVFILT_WRITE, kevents, nkev);
                    if (return_val)
                        ready_request(current);
                    else {
                        KEV_CTL(current->fd, EVFILT_WRITE, EV_ADD);
                    }
                    break;
                case DEAD:
                    ready_request(current);
                    break;
                default:
                    KEVENT_ISSET(current->fd, EVFILT_READ, kevents, nkev);
                    if (return_val)
                        ready_request(current);
                    else {
                        KEV_CTL(current->fd, EVFILT_READ, EV_ADD);
                    }
                    break;
            }
        }
        current = next;
    }
}
