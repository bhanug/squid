/*
 * $Id: ESISequence.cc,v 1.5 2005/07/03 15:25:08 serassio Exp $
 *
 * DEBUG: section 86    ESI processing
 * AUTHOR: Robert Collins
 *
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
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
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 * Copyright (c) 2003, Robert Collins <robertc@squid-cache.org>
 */

#include "squid.h"
#include "ESISequence.h"
#include "ESILiteral.h"
#include "ESIAttempt.h"
#include "ESIExcept.h"

class esiExcept;

esiSequence::~esiSequence ()
{
    debug (86,5)("esiSequence::~esiSequence %p\n", this);
}

esiSequence::esiSequence(esiTreeParentPtr aParent, bool incrementalFlag) : elements(), parent (aParent), mayFail_(true), failed (false), provideIncrementalData (incrementalFlag), processing (false), processingResult (ESI_PROCESS_COMPLETE), nextElementToProcess_ (0)
{}

size_t
esiSequence::nextElementToProcess() const
{
    return nextElementToProcess_;
}

void
esiSequence::nextElementToProcess(size_t const &aSizeT)
{
    nextElementToProcess_ = aSizeT;
}

bool
esiSequence::finishedProcessing() const
{
    return nextElementToProcess() >= elements.size();
}

bool
esiSequence::mayFail () const
{
    if (failed)
        return true;

    return mayFail_;
}

void
esiSequence::wontFail()
{
    assert (!failed);
    mayFail_ = false;
}

void
esiSequence::render(ESISegment::Pointer output)
{
    /* append all processed elements, and trim processed
     * and rendered elements 
     */
    assert (output->next == NULL);
    debugs (86,5, "esiSequenceRender: rendering " << processedcount << " elements");

    for (size_t i = 0; i < processedcount; ++i) {
        elements[i]->render(output);
        elements.setNULL(i,i+1);
        /* FIXME: pass a ESISegment ** ? */
        output = output->tail();
    }

    elements.pop_front (processedcount);
    processedcount = 0;
    assert (output->next == NULL);
}

void
esiSequence::finish()
{
    debug (86,5) ("esiSequence::finish: %p is finished\n", this);
    elements.setNULL(0, elements.size());
    parent = NULL;
}


void
esiSequence::provideData (ESISegment::Pointer data, ESIElement *source)
{
    ESIElement::Pointer lockthis = this;

    if (processing)
        debug (86,5) ("esiSequence::provideData: %p data provided during processing\n", this);

    debug (86,5) ("esiSequence::provideData %p %p %p\n", this, data.getRaw(), source);

    /* when data is provided, the element *must* be completed */
    /* XXX: when the callback model is complete,
     * we can introduce 'finished'. And then this rule can be 
     * relaxed
     */
    /* find the index */
    int index = elementIndex (source);

    assert (index >= 0);

    /* remove the current node */
    elements.setNULL(index, index+1);

    /* create a literal */
    esiLiteral *temp = new esiLiteral (data);

    /* insert the literal */
    elements[index] = temp;

    /* XXX: TODO push any pushable data upwards */
    /* fail() not done */
    if (processing)
        return;

    assert (process (flags.dovars) != ESI_PROCESS_FAILED);
}

bool
esiSequence::addElement (ESIElement::Pointer element)
{
    /* add an element to the output list */
    /* Some elements require specific parents */

    if (dynamic_cast<esiAttempt*>(element.getRaw()) ||
            dynamic_cast<esiExcept*>(element.getRaw())) {
        debug (86,0)("esiSequenceAdd: misparented Attempt or Except element (section 3.4)\n");
        return false;
    }

    /* Tie literals together for efficiency */
    if (elements.size() && dynamic_cast<esiLiteral*>(element.getRaw()) &&
            dynamic_cast<esiLiteral*>(elements[elements.size() - 1].getRaw())) {
        debug (86,5)("esiSequenceAdd: tying Literals %p and %p together\n", elements[elements.size() - 1].getRaw(),
                     element.getRaw());
        ESISegment::ListTransfer (((esiLiteral *)element.getRaw())->buffer,
                                  ((esiLiteral *)elements[elements.size() - 1].getRaw())->buffer);
        return true;
    }

    elements.push_back(element);
    debugs (86,3, "esiSequenceAdd: Added a new element, elements = " << elements.size());
    return true;
}

int
esiSequence::elementIndex(ESIElement::Pointer anElement) const
{
    for (size_t i = 0; i < elements.size(); ++i)
        if (elements[i] == anElement)
            return i;

    return -1;
}

void
esiSequence::processStep(int dovars)
{
    size_t elementToProcess = nextElementToProcess();
    nextElementToProcess(elementToProcess + 1);
    esiProcessResult_t tempResult = processOne(dovars, elementToProcess);

    if (processingResult < tempResult) {
        debug (86,5)("esiSequence::process: processingResult was %d, increasing to %d\n", processingResult, tempResult);
        processingResult = tempResult;
    }
}

esiProcessResult_t
esiSequence::processOne(int dovars, size_t index)
{
    debugs (86,5, "esiSequence::process " << this << " about to process element[" << index << "] " << elements[index].getRaw());

    switch (elements[index]->process(dovars)) {

    case ESI_PROCESS_COMPLETE:
        debug (86,5)("esiSequenceProcess: %p element %p Processed OK\n",
                     this, elements[index].getRaw());

        if (index == processedcount)
            /* another completely ready */
            ++processedcount;

        return ESI_PROCESS_COMPLETE;

    case ESI_PROCESS_PENDING_WONTFAIL:
        debug (86,5)("esiSequenceProcess: element Processed PENDING OK\n");

        return ESI_PROCESS_PENDING_WONTFAIL;

    case ESI_PROCESS_PENDING_MAYFAIL:
        debug (86,5)("eseSequenceProcess: element Processed PENDING UNKNOWN\n");

        return ESI_PROCESS_PENDING_MAYFAIL;

    case ESI_PROCESS_FAILED:
        debug (86,5)("esiSequenceProcess: element Processed FAILED\n");

        return ESI_PROCESS_FAILED;

    default:
        fatal ("unexpected code in esiSequence::processOne\n");

        return ESI_PROCESS_FAILED;
    }
}

esiProcessResult_t
esiSequence::process (int inheritedVarsFlag)
{
    debug (86,5) ("esiSequence::process: %p processing\n", this);

    if (processing) {
        debug (86,5)("esiSequence::process: %p reentry attempt during processing\n", this);
    }

    /* process as much of the list as we can, stopping only on
     * faliures
     */
    if (!processing || processedcount == 0)
        processingResult = ESI_PROCESS_COMPLETE;

    int dovars = inheritedVarsFlag;

    if (flags.dovars)
        dovars = 1;

    debug (86,5)("esiSequence::process: Processing %p with%s variable processing\n", this, dovars ? "" : "out");

    processing = true;

    nextElementToProcess(processedcount);

    while (!finishedProcessing()) {
        processStep(dovars);

        if (!processing)
            return processingResult;

        if (processingResult == ESI_PROCESS_FAILED) {
            elements.setNULL (0, elements.size());
            failed = true;
            parent = NULL;
            processing = false;
            return processingResult;
        }
    }

    assert (processingResult != ESI_PROCESS_COMPLETE || processedcount == elements.size());

    if (processingResult == ESI_PROCESS_COMPLETE || processingResult == ESI_PROCESS_PENDING_WONTFAIL)
        wontFail();

    if (processedcount == elements.size() || provideIncrementalData) {
        ESISegment::Pointer temp(new ESISegment);
        render (temp);

        if (temp->next.getRaw() || temp->len)
            parent->provideData(temp, this);
        else
            ESISegmentFreeList (temp);
    }

    /* Depends on full parsing before processing */
    if (processedcount == elements.size())
        parent = NULL;

    debug (86,5)("esiSequence::process: %p completed\n", this);

    processing = false;

    return processingResult;
}

void
esiSequence::fail (ESIElement *source, char const *anError)
{
    failed = true;

    if (processing) {
        debug (86,5) ("esiSequence::fail: %p failure callback during processing\n", this);
        return;
    }

    debug (86,5)("esiSequence::fail: %p has failed.\n", this);
    parent->fail (this, anError);
    elements.setNULL(0, elements.size());
    parent = NULL;
}

esiSequence::esiSequence(esiSequence const &old)
        : processedcount (0), mayFail_(old.mayFail_), failed (old.failed), provideIncrementalData (old.provideIncrementalData), processing (false), nextElementToProcess_ (0)
{
    flags.dovars = old.flags.dovars;
    parent = NULL;
}

void
esiSequence::makeCachableElements(esiSequence const &old)
{
    for (size_t counter = 0; counter < old.elements.size(); ++counter) {
        ESIElement::Pointer newElement = old.elements[counter]->makeCacheable();

        if (newElement.getRaw())
            assert (addElement(newElement));
    }
}

void
esiSequence::makeUsableElements(esiSequence const &old, ESIVarState &newVarState)
{
    for (size_t counter = 0; counter < old.elements.size(); ++counter) {
        ESIElement::Pointer newElement = old.elements[counter]->makeUsable (this, newVarState);

        if (newElement.getRaw())
            assert (addElement(newElement));
    }
}

ESIElement::Pointer
esiSequence::makeCacheable() const
{
    debug (86,5)("esiSequence::makeCacheable: Making cachable sequence from %p\n", this);
    assert (processedcount == 0);
    assert (!failed);

    if (elements.size() == 0) {
        debug (86,5)("esiSequence::makeCacheable: No elements in sequence %p, returning NULL\n", this);
        return NULL;
    }

    esiSequence * resultS = new esiSequence (*this);
    ESIElement::Pointer result = resultS;
    resultS->makeCachableElements(*this);
    debug (86,5)("esiSequence::makeCacheable: %p created %p\n", this, result.getRaw());
    return result;
}

ESIElement::Pointer
esiSequence::makeUsable(esiTreeParentPtr newParent, ESIVarState &newVarState) const
{
    debug (86,5)("esiSequence::makeUsable: Creating usable Sequence\n");
    assert (processedcount == 0);
    assert (!failed);

    if (elements.size() == 0) {
        debug (86,5)("esiSequence::makeUsable: No elements in sequence %p, returning NULL\n", this);
        return NULL;
    }

    esiSequence * resultS = new esiSequence (*this);
    ESIElement::Pointer result = resultS;
    resultS->parent = newParent;
    resultS->makeUsableElements(*this, newVarState);
    return result;
}

