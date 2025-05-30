/*
 * Phoenix-RTOS
 *
 * Operating system kernel
 *
 * ehci/ehci.c
 *
 * Copyright 2018, 2021, 2024 Phoenix Systems
 * Copyright 2007 Pawel Pisarczyk
 * Author: Jan Sikorski, Maciej Purski, Adam Greloch
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/mman.h>
#include <sys/threads.h>
#include <sys/list.h>
#include <sys/interrupt.h>
#include <sys/minmax.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <usbhost.h>
#include <stdio.h>

#include <board_config.h>

#include <hub.h>
#include <hcd.h>

#include "ehci.h"

#ifdef EHCI_IMX
#define EHCI_PERIODIC_SIZE 128
#else
#define EHCI_PERIODIC_SIZE 1024
#endif

#ifndef EHCI_PRIO
#define EHCI_PRIO 2
#endif


static inline void ehci_memDmb(void)
{
#ifdef EHCI_IMX
	asm volatile("dmb" ::: "memory");
#else
	__sync_synchronize();
#endif
}


static void ehci_startAsync(hcd_t *hcd)
{
	ehci_t *ehci = (ehci_t *)hcd->priv;

	*(ehci->opbase + asynclistaddr) = va2pa((void *)ehci->asyncList->hw);
	*(ehci->opbase + usbcmd) |= USBCMD_ASE;
	ehci_memDmb();
	while ((*(ehci->opbase + usbsts) & USBSTS_AS) == 0)
		;
}


static void ehci_stopAsync(hcd_t *hcd)
{
	ehci_t *ehci = (ehci_t *)hcd->priv;

	*(ehci->opbase + usbcmd) &= ~USBCMD_ASE;
	ehci_memDmb();
	while ((*(ehci->opbase + usbsts) & USBSTS_AS) != 0)
		;
}


static void ehci_qtdLink(ehci_qtd_t *prev, ehci_qtd_t *next)
{
	prev->hw->next = next->paddr;
	ehci_memDmb();
}


static void ehci_enqueue(hcd_t *hcd, ehci_qh_t *qh, ehci_qtd_t *first, ehci_qtd_t *last)
{
	ehci_t *ehci = (ehci_t *)hcd->priv;

	mutexLock(ehci->asyncLock);
	last->hw->next = QTD_PTR_INVALID;
	last->hw->token |= QTD_IOC;

	/* No qtds linked */
	if (qh->lastQtd == NULL)
		qh->hw->nextQtd = first->paddr;
	else
		qh->lastQtd->next = first->paddr;
	ehci_memDmb();

	qh->lastQtd = last->hw;
	mutexUnlock(ehci->asyncLock);
}


static void ehci_continue(ehci_t *ehci, ehci_qh_t *qh, ehci_qtd_t *last)
{
	mutexLock(ehci->asyncLock);
	/* Queue for this qh is empty */
	if (qh->lastQtd == last->hw) {
		qh->lastQtd = NULL;
		qh->hw->nextQtd = QTD_PTR_INVALID;
	}

	/* The queue got stuck, as the last qtd has been updated, when the qh had a copy in overlay area */
	if (qh->hw->nextQtd == QTD_PTR_INVALID && (qh->hw->current == last->paddr))
		qh->hw->nextQtd = last->hw->next;

	/* Cleanup error state */
	if (qh->hw->token & QTD_ERRMASK) {
		qh->hw->nextQtd = last->hw->next;
		qh->hw->token &= ~0x7e;
	}
	ehci_memDmb();
	mutexUnlock(ehci->asyncLock);
}


static ehci_qtd_t *ehci_qtdGet(ehci_t *ehci)
{
	ehci_qtd_t *qtd;

	mutexLock(ehci->asyncLock);
	qtd = ehci->qtdPool;

	if (qtd != NULL) {
		LIST_REMOVE(&ehci->qtdPool, qtd);
		ehci->nqtds--;
	}
	mutexUnlock(ehci->asyncLock);

	return qtd;
}


static void ehci_qtdsPut(ehci_t *ehci, ehci_qtd_t **head)
{
	ehci_qtd_t *q, *t;

	mutexLock(ehci->asyncLock);
	while ((q = *head) != NULL) {
		LIST_REMOVE(head, q);

		if (ehci->nqtds >= EHCI_MAX_QTD_POOL) {
			t = ehci->qtdPool;
			LIST_REMOVE(&ehci->qtdPool, t);
			usb_free((void *)t->hw, sizeof(*t->hw));
			free(t);
			ehci->nqtds--;
		}
		LIST_ADD(&ehci->qtdPool, q);
		q->qh = NULL;
		ehci->nqtds++;
	}
	mutexUnlock(ehci->asyncLock);
}


static void
ehci_qtdDump(ehci_qtd_t *qtd, bool dump_bufs)
{
#if EHCI_DEBUG_QTD
	uint32_t s;

	s = qtd->hw->token;
	fprintf(stderr, "sts=0x%08x: tog=%d sz=0x%x ioc=%d",
			s, s >> 31, (s >> 16) & 0x7fff,
			(s >> 15) & 0b1);
	fprintf(stderr, " cerr=%d pid=%d %s%s%s%s%s%s%s%s\n",
			(s >> 10) & 0b11, (s >> 8) & 0b11,
			(s & QTD_ACTIVE) ? "ACTIVE" : "NOT_ACTIVE",
			(s & QTD_HALTED) ? "-HALTED" : "",
			(s & QTD_BUFERR) ? "-BUFERR" : "",
			(s & QTD_BABBLE) ? "-BABBLE" : "",
			(s & QTD_XACT) ? "-XACT" : "",
			(s & QTD_MISSED_UFRAME) ? "-MISSED" : "",
			(s & QTD_SPLIT) ? "-SPLIT" : "",
			(s & QTD_PING) ? "-PING" : "");

	for (s = 0; dump_bufs && s < EHCI_QH_NBUFS; s++) {
		fprintf(stderr, "  buf[%d]=0x%08x  buf_hi[%d]=0x%08x\n", s, qtd->hw->buf[s], s, qtd->hw->buf_hi[s]);
	}
#endif
}


static ehci_qtd_t *ehci_qtdAlloc(ehci_t *ehci, int pid, size_t maxpacksz, char *data, size_t *size, int datax)
{
	ehci_qtd_t *qtd;
	size_t bytes = 0;
	int i, offs;

	/* Try to reuse a qtd */
	if ((qtd = ehci_qtdGet(ehci)) == NULL) {
		if ((qtd = malloc(sizeof(ehci_qtd_t))) == NULL)
			return NULL;

		if ((qtd->hw = usb_alloc(sizeof(struct qtd))) == NULL) {
			free(qtd);
			return NULL;
		}
		qtd->paddr = QTD_PTR(qtd);
	}

	qtd->hw->token = (datax << 31) | (pid << 8) | (EHCI_TRANS_ERRORS << 10) | QTD_ACTIVE;

	qtd->hw->next = QTD_PTR_INVALID;
	qtd->hw->altnext = QTD_PTR_INVALID;

	if (data != NULL) {
		qtd->hw->buf[0] = (uintptr_t)va2pa(data);
		qtd->hw->buf_hi[0] = 0;

		offs = min(EHCI_PAGE_SIZE - QTD_OFFSET(qtd->hw->buf[0]), *size);
		bytes += offs;
		data += offs;

		for (i = 1; i < EHCI_QH_NBUFS && bytes != *size; i++) {
			qtd->hw->buf[i] = va2pa(data) & ~0xfff;
			qtd->hw->buf_hi[i] = 0;

			offs = min(*size - bytes, EHCI_PAGE_SIZE);
			/* If the data does not fit one qtd, don't leave a trailing short packet */
			if (i == EHCI_QH_NBUFS - 1 && bytes + offs < *size)
				offs = (((bytes + offs) / maxpacksz) * maxpacksz) - bytes;

			bytes += offs;
			data += offs;
		}

		for (; i < EHCI_QH_NBUFS; i++) {
			qtd->hw->buf[i] = 0;
			qtd->hw->buf_hi[i] = 0;
		}

		qtd->hw->token |= bytes << 16;
		*size -= bytes;
	}

	return qtd;
}


static ehci_qh_t *ehci_qhGet(ehci_t *ehci)
{
	ehci_qh_t *qh;

	mutexLock(ehci->asyncLock);
	qh = ehci->qhPool;

	if (qh != NULL) {
		LIST_REMOVE(&ehci->qhPool, qh);
		ehci->nqhs--;
	}
	mutexUnlock(ehci->asyncLock);

	return qh;
}


static void ehci_qhPut(ehci_t *ehci, ehci_qh_t *qh)
{
	ehci_qh_t *t;

	mutexLock(ehci->asyncLock);

	if (ehci->nqhs >= EHCI_MAX_QH_POOL) {
		t = ehci->qhPool;
		LIST_REMOVE(&ehci->qhPool, t);
		usb_free((void *)t->hw, sizeof(*t->hw));
		free(t);
		ehci->nqhs--;
	}

	LIST_ADD(&ehci->qhPool, qh);
	ehci->nqhs++;
	mutexUnlock(ehci->asyncLock);
}


static ehci_qh_t *ehci_qhAlloc(ehci_t *ehci)
{
	ehci_qh_t *qh;
	int i;

	if ((qh = ehci_qhGet(ehci)) == NULL) {
		if ((qh = malloc(sizeof(ehci_qh_t))) == NULL)
			return NULL;

		if ((qh->hw = usb_alloc(sizeof(struct qh))) == NULL) {
			free(qh);
			return NULL;
		}
	}

	qh->hw->info[0] = 0;
	qh->hw->info[1] = 0;
	qh->hw->token = 0;
	qh->hw->horizontal = QH_PTR_INVALID;
	qh->hw->current = QTD_PTR_INVALID;
	qh->hw->nextQtd = QTD_PTR_INVALID;
	qh->hw->altnextQtd = QTD_PTR_INVALID;

	qh->next = NULL;
	qh->prev = NULL;
	qh->period = 0;
	qh->uframe = 0;
	qh->phase = 0;
	qh->lastQtd = NULL;

	for (i = 0; i < EHCI_QH_NBUFS; i++) {
		qh->hw->buf[i] = 0;
		qh->hw->buf_hi[i] = 0;
	}

	return qh;
}


static void ehci_qhConf(ehci_qh_t *qh, usb_pipe_t *pipe)
{
	qh->hw->info[0] = pipe->dev->address;
	qh->hw->info[0] |= (pipe->num << 8);
	qh->hw->info[0] |= (pipe->dev->speed << 12);
	qh->hw->info[0] |= (pipe->type == usb_transfer_control) ? QH_DT : 0;
	qh->hw->info[0] |= (pipe->maxPacketLen << 16);

	if (pipe->type == usb_transfer_control && pipe->dev->speed != usb_high_speed)
		qh->hw->info[0] |= QH_CTRL;

	qh->hw->info[0] |= (3 << 28); /* NAK count reload */
	qh->hw->info[1] = 0;

	if (pipe->type == usb_transfer_interrupt) {
		if (pipe->dev->speed == usb_high_speed) {
			qh->period = ((1 << (pipe->interval - 1))) >> 3;
			/* Assume, that for 1-8 microframes period, we send it every microframe */
			if (qh->period == 0)
				qh->period = 1;
		}
		else {
			qh->period = 1;
			while (qh->period * 2 < pipe->interval)
				qh->period *= 2;
		}
	}
}


static void ehci_bandAlloc(ehci_t *ehci, ehci_qh_t *qh)
{
	ehci_qh_t *tmp;
	unsigned int i, n, best;
	unsigned int ucnt[8] = { 0 };

	best = (unsigned)-1;
	qh->phase = 0;
	qh->uframe = 0xff;

	/* Find the best periodicList index (phase) to begin Qh linking */
	for (i = 0; i < qh->period && i < EHCI_PERIODIC_SIZE; i++) {
		n = 0;
		/* Count Qhs linked to this periodic index */
		tmp = ehci->periodicNodes[i];
		while (tmp != NULL) {
			n++;
			tmp = tmp->next;
		}

		if (n < best) {
			best = n;
			qh->phase = i;
		}
	}

	/* Find the best microframe in a frame. For periods equal to 1, send it every microframe */
	if ((qh->hw->info[0] & QH_HIGH_SPEED) && qh->period > 1) {
		for (tmp = ehci->periodicNodes[qh->phase]; tmp != NULL; tmp = tmp->next) {
			if (tmp->uframe != 0xff)
				ucnt[tmp->uframe]++;
		}

		best = (unsigned)-1;
		qh->uframe = 0;
		for (i = 0; i < 8; i++) {
			if (ucnt[i] < best) {
				qh->uframe = i;
				best = ucnt[i];
			}
		}
	}
}


static void ehci_qhLinkPeriodic(hcd_t *hcd, ehci_qh_t *qh)
{
	ehci_t *ehci = (ehci_t *)hcd->priv;
	ehci_qh_t *t;
	int i;

	mutexLock(ehci->periodicLock);
	ehci_bandAlloc(ehci, qh);
	qh->hw->info[1] = (qh->uframe != 0xff) ? (1 << qh->uframe) : QH_SMASK;
	qh->hw->info[1] |= QH_CMASK;
	/* TODO: Handle SPLIT transactions */

	t = ehci->periodicNodes[qh->phase];
	while (t != NULL && t->next != NULL && t->next->period >= qh->period)
		t = t->next;

	if (t == NULL || t->period < qh->period) {
		/* New first element */
		qh->next = ehci->periodicNodes[qh->phase];

		for (i = qh->phase; i < EHCI_PERIODIC_SIZE; i += qh->period) {
			ehci->periodicNodes[i] = qh;
			ehci->periodicList[i] = QH_PTR(qh);
		}
	}
	else {
		/* Insert inside */
		qh->next = t->next;
		t->next = qh;
		qh->hw->horizontal = QH_PTR(qh->next);
		t->hw->horizontal = QH_PTR(qh);
	}

	if (qh->next == NULL) {
		/* New last element */
		qh->hw->horizontal |= QH_PTR_INVALID;
	}
	ehci_memDmb();
	mutexUnlock(ehci->periodicLock);
}


static void ehci_qhLinkAsync(hcd_t *hcd, ehci_qh_t *qh)
{
	ehci_t *ehci = (ehci_t *)hcd->priv;

	mutexLock(ehci->asyncLock);

	/* Insert after dummy qh */
	qh->next = ehci->asyncList->next;
	qh->prev = ehci->asyncList;

	qh->next->prev = qh;
	ehci->asyncList->next = qh;

	qh->hw->horizontal = ehci->asyncList->hw->horizontal;
	ehci->asyncList->hw->horizontal = QH_PTR(qh);
	ehci_memDmb();

	mutexUnlock(ehci->asyncLock);
}


static void ehci_qtdsDeactivate(ehci_qtd_t *qtds)
{
	ehci_qtd_t *e = qtds;

	if (e != NULL) {
		do {
			e->hw->token &= ~QTD_ACTIVE;
		} while ((e = e->next) != qtds);
	}
	ehci_memDmb();
}


static void ehci_qhUnlinkAsync(hcd_t *hcd, ehci_qh_t *qh)
{
	ehci_t *ehci = (ehci_t *)hcd->priv;

	mutexLock(ehci->asyncLock);

	ehci_stopAsync(hcd);
	qh->prev->hw->horizontal = qh->hw->horizontal;
	ehci_startAsync(hcd);
	ehci_memDmb();

	qh->prev->next = qh->next;
	qh->next->prev = qh->prev;

	mutexUnlock(ehci->asyncLock);
}


void ehci_qhUnlinkPeriodic(hcd_t *hcd, ehci_qh_t *qh)
{
	ehci_t *ehci = (ehci_t *)hcd->priv;
	ehci_qh_t *tmp;
	int i;

	mutexLock(ehci->periodicLock);
	/* TODO: do we have to stop the periodic queue? */
	for (i = 0; i < EHCI_PERIODIC_SIZE; i++) {
		/* Count Qhs linked to this periodic index */
		tmp = ehci->periodicNodes[i];

		if (tmp == qh) {
			if (qh->next != NULL) {
				ehci->periodicList[i] = QH_PTR(qh->next);
			}
			else {
				ehci->periodicList[i] = QH_PTR_INVALID;
			}
			ehci->periodicNodes[i] = qh->next;
		}
		else {
			while (tmp != NULL && tmp->next != qh)
				tmp = tmp->next;

			if (tmp != NULL && tmp->next == qh) {
				tmp->next = qh->next;
				if (tmp->next != NULL) {
					tmp->hw->horizontal = QH_PTR(tmp->next);
				}
				else {
					tmp->hw->horizontal = QH_PTR_INVALID;
				}
			}
		}
	}
	ehci_memDmb();
	mutexUnlock(ehci->periodicLock);
}


static int ehci_irqHandler(unsigned int n, void *data)
{
	hcd_t *hcd = (hcd_t *)data;
	ehci_t *ehci = (ehci_t *)hcd->priv;
	uint32_t currentStatus;

	currentStatus = *(ehci->opbase + usbsts);
	do {
		*(ehci->opbase + usbsts) = currentStatus & (EHCI_INTRMASK | USBSTS_FRI);

		ehci->status |= currentStatus;

		/* For edge triggered interrupts to prevent losing interrupts,
		 * poll the usbsts register until it is stable */
		currentStatus = *(ehci->opbase + usbsts);
	} while ((currentStatus & EHCI_INTRMASK) != 0);

	return -!(ehci->status & EHCI_INTRMASK);
}


static int ehci_qtdsCheck(hcd_t *hcd, usb_transfer_t *t, int *status)
{
	ehci_qtd_t *qtds = (ehci_qtd_t *)t->hcdpriv;
	int error = 0;
	int finished = 0;

	*status = 0;
	do {
		ehci_qtdDump(qtds, false);
		if (qtds->hw->token & (QTD_XACT | QTD_BABBLE | QTD_BUFERR | QTD_HALTED)) {
			error++;
		}

		qtds = qtds->next;
	} while (qtds != t->hcdpriv);

	if (error > 0) {
		finished = 1;
		*status = -error;
	}

	/* Finished no error */
	if (!(qtds->prev->hw->token & QTD_ACTIVE) || (qtds->prev->hw->token & QTD_HALTED)) {
		finished = 1;
		*status = t->size - QTD_LEN(qtds->prev->hw->token);
	}

	return finished;
}


static void ehci_transUpdate(hcd_t *hcd)
{
	ehci_qh_t *qh;
	ehci_qtd_t *qtd;
	usb_transfer_t *t, *n;
	int cont;
	int status;

	if ((t = hcd->transfers) == NULL)
		return;

	do {
		qtd = (ehci_qtd_t *)t->hcdpriv;
		qh = qtd->qh;
		cont = 0;
		n = t->next;

		if (ehci_qtdsCheck(hcd, t, &status)) {
			ehci_continue(hcd->priv, qh, qtd->prev);
			ehci_qtdsPut(hcd->priv, &qtd);
			LIST_REMOVE(&hcd->transfers, t);
			t->hcdpriv = NULL;
			usb_transferFinished(t, status);
			if (n != t)
				cont = 1;
		}
	} while (hcd->transfers && ((t = n) != hcd->transfers || cont));
}


static void ehci_portStatusChanged(hcd_t *hcd)
{
	usb_dev_t *hub = hcd->roothub;
	uint32_t status;

	status = ehci_getHubStatus(hub);

	if (status != 0 && !usb_transferCheck(hub->statusTransfer)) {
		memcpy(hub->statusTransfer->buffer, &status, sizeof(status));
		usb_transferFinished(hub->statusTransfer, hub->statusTransfer->size);
	}
}


#if EHCI_DEBUG_IRQ
static void ehci_printIrq(hcd_t *hcd)
{
	ehci_t *ehci = (ehci_t *)hcd->priv;
	static char buf[30];
	size_t i = 0;

	i += sprintf(buf, "INT%d: ", hcd->info->irq);

#define append_to_buf(interrupt) \
	if (ehci->status & (interrupt)) { \
		i += sprintf(buf + i, #interrupt " "); \
	}
	append_to_buf(USBSTS_UI);
	append_to_buf(USBSTS_UEI);
	append_to_buf(USBSTS_SEI);
	append_to_buf(USBSTS_PCI);

	log_debug("%s", buf);
}
#endif


static void ehci_irqThread(void *arg)
{
	hcd_t *hcd = (hcd_t *)arg;
	ehci_t *ehci = (ehci_t *)hcd->priv;

	mutexLock(ehci->irqLock);
	for (;;) {
		condWait(ehci->irqCond, ehci->irqLock, 0);

#if EHCI_DEBUG_IRQ
		ehci_printIrq(hcd);
#endif

		/* The irqThread must clear the handler interrupt status,
			 since otherwise it would handle ghost interrupts
			 on every interrupt (irqHandler never clears ehci->status) */
		if (ehci->status & USBSTS_SEI) {
			ehci->status &= ~USBSTS_SEI;
			log_error("host system error, controller halted");
			/* TODO cleanup/reset after death */
			continue;
		}

		if (ehci->status & (USBSTS_UI | USBSTS_UEI)) {
			ehci->status &= ~(USBSTS_UI | USBSTS_UEI);
			mutexLock(hcd->transLock);
			ehci_transUpdate(hcd);
			mutexUnlock(hcd->transLock);
		}

		if (ehci->status & USBSTS_PCI) {
			ehci->status &= ~USBSTS_PCI;
			ehci_portStatusChanged(hcd);
		}
	}
}


static int ehci_qtdAdd(ehci_t *ehci, ehci_qtd_t **list, int token, size_t maxpacksz, char *buf, size_t size, int dt)
{
	ehci_qtd_t *tmp;
	size_t remaining = size;

	do {
		if ((tmp = ehci_qtdAlloc(ehci, token, maxpacksz, buf + size - remaining, &remaining, dt)) == NULL)
			return -ENOMEM;

		LIST_ADD(list, tmp);
		dt = 1 - dt;
	} while (remaining > 0);

	return 0;
}


static void ehci_transferDequeue(hcd_t *hcd, usb_transfer_t *t)
{
	mutexLock(hcd->transLock);
	/* note: not tested for interrupt transfers */
	if (t->hcdpriv != NULL)
		ehci_qtdsDeactivate((ehci_qtd_t *)t->hcdpriv);
	ehci_transUpdate(hcd);
	mutexUnlock(hcd->transLock);
}


static int ehci_transferEnqueue(hcd_t *hcd, usb_transfer_t *t, usb_pipe_t *pipe)
{
	ehci_qh_t *qh;
	ehci_qtd_t *qtds = NULL;
	int token = t->direction == usb_dir_in ? in_token : out_token;

	if (usb_isRoothub(pipe->dev))
		return ehci_roothubReq(pipe->dev, t);

	if (pipe->hcdpriv == NULL) {
		if ((qh = ehci_qhAlloc(hcd->priv)) == NULL)
			return -ENOMEM;

		ehci_qhConf(qh, pipe);
		pipe->hcdpriv = qh;

		if (t->type == usb_transfer_bulk || t->type == usb_transfer_control)
			ehci_qhLinkAsync(hcd, qh);
		else
			ehci_qhLinkPeriodic(hcd, qh);
	}
	else {
		qh = (ehci_qh_t *)pipe->hcdpriv;

		/* Update fields, which might have been changed */
		if (QH_DEVADDR(qh->hw->info[0]) != pipe->dev->address)
			qh->hw->info[0] = (qh->hw->info[0] & ~0x7f) | pipe->dev->address;

		if (QH_PACKLEN(qh->hw->info[0]) != pipe->maxPacketLen)
			qh->hw->info[0] = (qh->hw->info[0] & ~(0x7ff << 16)) | (pipe->maxPacketLen << 16);
	}

	/* Setup stage */
	if (t->type == usb_transfer_control) {
		if (ehci_qtdAdd(hcd->priv, &qtds, setup_token, pipe->maxPacketLen, (char *)t->setup, sizeof(usb_setup_packet_t), 0) < 0) {
			ehci_qtdsPut(hcd->priv, &qtds);
			t->hcdpriv = NULL;
			return -ENOMEM;
		}
	}

	/* Data stage */
	if ((t->type == usb_transfer_control && t->size > 0) || t->type == usb_transfer_bulk ||
			t->type == usb_transfer_interrupt) {
		if (ehci_qtdAdd(hcd->priv, &qtds, token, pipe->maxPacketLen, t->buffer, t->size, 1) < 0) {
			ehci_qtdsPut(hcd->priv, &qtds);
			t->hcdpriv = NULL;
			return -ENOMEM;
		}
	}

	/* Status stage */
	if (t->type == usb_transfer_control) {
		token = (token == in_token) ? out_token : in_token;
		if (ehci_qtdAdd(hcd->priv, &qtds, token, pipe->maxPacketLen, NULL, 0, 1) < 0) {
			ehci_qtdsPut(hcd->priv, &qtds);
			t->hcdpriv = NULL;
			return -ENOMEM;
		}
	}

	/* No qtds allocated */
	if (qtds == NULL)
		return -1;

	t->hcdpriv = qtds;
	do {
		ehci_qtdLink(qtds, qtds->next);
		qtds->qh = qh;
		qtds = qtds->next;
	} while (qtds != t->hcdpriv);

	qtds = (ehci_qtd_t *)t->hcdpriv;

	mutexLock(hcd->transLock);
	LIST_ADD(&hcd->transfers, t);
	ehci_enqueue(hcd, qh, qtds, qtds->prev);
	mutexUnlock(hcd->transLock);

	return 0;
}


static void ehci_pipeDestroy(hcd_t *hcd, usb_pipe_t *pipe)
{
	usb_transfer_t *t;
	ehci_qh_t *qh;
	ehci_qtd_t *qtds;

	if (pipe->hcdpriv == NULL)
		return;

	qh = (ehci_qh_t *)pipe->hcdpriv;

	if (pipe->type == usb_transfer_bulk || pipe->type == usb_transfer_control)
		ehci_qhUnlinkAsync(hcd, qh);
	else if (pipe->type == usb_transfer_interrupt)
		ehci_qhUnlinkPeriodic(hcd, qh);

	mutexLock(hcd->transLock);
	t = hcd->transfers;
	/* Deactivate device's qtds */
	if (t != NULL) {
		do {
			qtds = (ehci_qtd_t *)t->hcdpriv;
			if (qtds->qh == pipe->hcdpriv)
				ehci_qtdsDeactivate(qtds);
			t = t->next;
		} while (t != hcd->transfers);
		ehci_transUpdate(hcd);
	}
	mutexUnlock(hcd->transLock);

	pipe->hcdpriv = NULL;
	ehci_qhPut(hcd->priv, qh);
}


static void ehci_free(ehci_t *ehci)
{
	if (ehci->periodicList != NULL)
		usb_freeAligned(ehci->periodicList, EHCI_PERIODIC_SIZE * sizeof(uint32_t));

	if (ehci->irqCond != 0)
		resourceDestroy(ehci->irqCond);

	if (ehci->irqLock != 0)
		resourceDestroy(ehci->irqLock);

	if (ehci->asyncLock != 0)
		resourceDestroy(ehci->asyncLock);

	free(ehci->periodicNodes);
	free(ehci);
}


static int ehci_init(hcd_t *hcd)
{
	ehci_t *ehci;
	ehci_qh_t *qh;
	int i, ret;

	if ((ehci = calloc(1, sizeof(ehci_t))) == NULL) {
		log_error("Out of memory!");
		return -ENOMEM;
	}

	if ((ehci->periodicList = usb_allocAligned(EHCI_PERIODIC_SIZE * sizeof(uint32_t), EHCI_PERIODIC_ALIGN)) == NULL) {
		log_error("Out of memory!");
		ehci_free(ehci);
		return -ENOMEM;
	}

	if ((ehci->periodicNodes = calloc(EHCI_PERIODIC_SIZE, sizeof(ehci_qh_t *))) == NULL) {
		log_error("Out of memory!");
		ehci_free(ehci);
		return -ENOMEM;
	}

	hcd->priv = ehci;

	if (phy_init(hcd) != 0) {
		log_error("Phy init failed!");
		ehci_free(ehci);
		return -EINVAL;
	}

	if (condCreate(&ehci->irqCond) < 0) {
		log_error("Out of memory!");
		ehci_free(ehci);
		return -ENOMEM;
	}

	if (mutexCreate(&ehci->irqLock) < 0) {
		log_error("Out of memory!");
		ehci_free(ehci);
		return -ENOMEM;
	}

	if (mutexCreate(&ehci->asyncLock) < 0) {
		log_error("Out of memory!");
		ehci_free(ehci);
		return -ENOMEM;
	}

	if (mutexCreate(&ehci->periodicLock) < 0) {
		log_error("Out of memory!");
		ehci_free(ehci);
		return -ENOMEM;
	}

	/* Initialize Async List with a dummy qh to optimize
	 * accesses and make them safer */
	if ((qh = ehci_qhAlloc(ehci)) == NULL) {
		log_error("Out of memory!");
		ehci_free(ehci);
		return -ENOMEM;
	}
	qh->hw->info[0] |= QH_HEAD;
	qh->hw->horizontal = QH_PTR(qh);
	LIST_ADD(&ehci->asyncList, qh);

	for (i = 0; i < EHCI_PERIODIC_SIZE; ++i)
		ehci->periodicList[i] = QH_PTR_INVALID;

	if (((addr_t)hcd->base & (0x20 - 1)) != 0) {
		log_error("USBBASE not aligned to 32 bits");
		ehci_free(ehci);
		return -EINVAL;
	}

	/* Set USBBASE */
	ehci->base = hcd->base;

#ifdef EHCI_IMX
	/* imx deviation: Here we don't distinguish between base/opbase addresses, as
	 * the distance between operational register base and USBBASE is a known
	 * constant accounted for in the register enum already. */
	ehci->opbase = ehci->base;
#else
	/* In general, EHCI states that the operational register base has address:
	 * USBBASE + CAPLENGTH */
	ehci->opbase = (volatile uint32_t *)((char *)ehci->base + *(uint8_t *)(ehci->base + caplength));
#endif

	log_debug("attaching handler to irq=%d", hcd->info->irq);
	ret = interrupt(hcd->info->irq, ehci_irqHandler, hcd, ehci->irqCond, &ehci->irqHandle);

	if (ret < 0) {
		log_error("failed to set interrupt handler");
		return ret;
	}

	if (beginthread(ehci_irqThread, EHCI_PRIO, ehci->stack, sizeof(ehci->stack), hcd) != 0) {
		ehci_free(ehci);
		return -ENOMEM;
	}

#ifndef EHCI_IMX
	/* Hangs controller on imx */
	*(ehci->opbase + usbcmd) &= ~(USBCMD_RUN | USBCMD_IAA);

	while ((*(ehci->opbase + usbsts) & USBSTS_HCH) == 0)
		;
#endif

	/* Reset controller */
	*(ehci->opbase + usbcmd) |= USBCMD_HCRESET;
	while ((*(ehci->opbase + usbcmd) & USBCMD_HCRESET) != 0)
		;

#ifdef EHCI_IMX
	/* imx deviation: Set host mode */
	*(ehci->opbase + usbmode) |= 3;
#else
	if ((*(ehci->base + hccparams) & HCCPARAMS_64BIT_ADDRS) != 0) {
		*(ehci->opbase + ctrldssegment) = 0;
	}
#endif

	/* Enable interrupts */
	*(ehci->opbase + usbintr) = USBSTS_UI | USBSTS_UEI | USBSTS_SEI;

	/* Set periodic frame list */
	*(ehci->opbase + periodiclistbase) = va2pa(ehci->periodicList);

#ifdef EHCI_IMX
	/* imx deviation: Set frame list size (128 elements) */
	*(ehci->opbase + usbcmd) |= (3 << 2);
#endif

	/* Turn the controller on, enable periodic scheduling */
	*(ehci->opbase + usbcmd) &= ~(USBCMD_LRESET | USBCMD_ASE);

	*(ehci->opbase + usbcmd) |= (USBCMD_PSE | USBCMD_RUN);
	while ((*(ehci->opbase + usbsts) & (USBSTS_HCH)) != 0)
		;

	/* Route all ports to this host controller */
	*(ehci->opbase + configflag) = 1;

	/* Allow for the hardware to catch up */
	usleep(50 * 1000);

	ehci_startAsync(hcd);

	log_debug("hc initialized");

	return 0;
}


static const hcd_ops_t ehci_ops = {
	.type = "ehci",
	.init = ehci_init,
	.transferEnqueue = ehci_transferEnqueue,
	.transferDequeue = ehci_transferDequeue,
	.pipeDestroy = ehci_pipeDestroy,
	.getRoothubStatus = ehci_getHubStatus
};


__attribute__((constructor)) static void ehci_register(void)
{
	hcd_register(&ehci_ops);
}
