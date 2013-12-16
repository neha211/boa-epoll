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
 * Name: EPOLL_ISSET
 *
 * Description: macro to check if the event has occurred.
 * Meant only for local use.
 */

#define EPOLL_ISSET(fd1, type, revents, count) {\
	int i=0;\
	return_val = 0;\
	for(i=0; i<=count; ++i) \
	  if(revents[i].data.fd == fd1 && revents[i].events == type) {\
	     return_val = 1; \
	     break; \
	} \
}

static void fdset_update(void);
int maxfd_count;
int efd;
int return_val;
struct epoll_event event; 		/* to register events for new fd */
struct epoll_event revents[MAX_EVENTS]; /* returned events on epoll_wait */

void epoll_loop(int server_s)
{
    /* Create the epoll instance */
    if ((efd = epoll_create(MAX_EVENTS)) < 0) {
		fprintf(stderr, "Error in creating epoll instance\n");
		DIE("epoll");
    }

    EPOLL_CTL(server_s, EPOLLIN, EPOLL_CTL_ADD);

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

        /* move selected req's from request_block to request_ready */
        if (request_block)
            fdset_update();

        /* any blocked req's move from request_ready to request_block */
        process_requests(server_s);

	int time_out = (request_ready ? 0 : (ka_timeout ? ka_timeout : REQUEST_TIMEOUT));

        maxfd_count = epoll_wait(efd, revents, MAX_EVENTS, (request_ready || request_block ? time_out : 0));
		
	if (maxfd_count == -1) {
	    if (errno == EINTR)
                continue;   /* while(1) */
            else if (errno != EBADF) {
                DIE("epoll_wait");
            }
        }
	
        time(&current_time);

        EPOLL_ISSET(server_s, EPOLLIN, revents, maxfd_count);
		
	if (return_val)        
		pending_requests = 1;
    }
}

/*
 * Name: fdset_update
 *
 * Description: iterate through the blocked requests, checking whether
 * that file descriptor has been set by the event.  Update the revents to
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
			EPOLL_ISSET(current->fd, EPOLLOUT, revents, maxfd_count);
            if (return_val)
                ready_request(current);
            else {
                EPOLL_CTL(current->fd, EPOLLOUT, EPOLL_CTL_ADD);
            }
        } else {
            switch (current->status) {
            case WRITE:
            case PIPE_WRITE:
				EPOLL_ISSET(current->fd, EPOLLOUT, revents, maxfd_count);
                if (return_val)
                    ready_request(current);
                else {
                    EPOLL_CTL(current->fd, EPOLLOUT, EPOLL_CTL_ADD);
                }
                break;
            case BODY_WRITE:
				EPOLL_ISSET(current->post_data_fd, EPOLLOUT, revents, maxfd_count);
                if (return_val)
                    ready_request(current);
                else {
                    EPOLL_CTL(current->post_data_fd, EPOLLOUT, EPOLL_CTL_ADD);
                }
                break;
            case PIPE_READ:
				EPOLL_ISSET(current->data_fd, EPOLLIN, revents, maxfd_count);
                if (return_val)
                    ready_request(current);
                else {
                    EPOLL_CTL(current->data_fd, EPOLLIN, EPOLL_CTL_ADD);
                }
                break;
            case DONE:
				EPOLL_ISSET(current->fd, EPOLLOUT, revents, maxfd_count);
                if (return_val)
                    ready_request(current);
                else {
                    EPOLL_CTL(current->fd, EPOLLOUT, EPOLL_CTL_ADD);
                }
                break;
            case DEAD:
                ready_request(current);
                break;
            default:
				EPOLL_ISSET(current->fd, EPOLLIN, revents, maxfd_count);
                if (return_val)
                    ready_request(current);
                else {
                    EPOLL_CTL(current->fd, EPOLLIN, EPOLL_CTL_ADD);
                }
                break;
            }
        }
        current = next;
    }
}
