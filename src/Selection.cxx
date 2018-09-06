/*
 * Copyright 2017-2018 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Selection.hxx"
#include "Record.hxx"

void
Selection::SkipMismatches() noexcept
{
	while (cursor && !filter(cursor->GetParsed()))
		++cursor;
}

bool
Selection::FixDeleted() noexcept
{
	if (!cursor.FixDeleted())
		return false;

	SkipMismatches();
	return true;
}

void
Selection::Rewind() noexcept
{
	assert(!cursor);
	assert(end_id == UINT64_MAX);

	if (filter.since != Net::Log::TimePoint::min() ||
	    filter.until != Net::Log::TimePoint::max()) {
		const auto tr = cursor.TimeRange(filter.since, filter.until);
		if (tr.first == nullptr)
			return;

		cursor.SetNext(*tr.first);

		if (tr.second != nullptr)
			end_id = tr.second->GetId();
	} else
		cursor.Rewind();

	SkipMismatches();
}

bool
Selection::OnAppend(const Record &record) noexcept
{
	assert(!*this);

	if (!filter(record.GetParsed()))
		return false;

	cursor.OnAppend(record);
	return true;
}

Selection::operator bool() const noexcept
{
	return cursor && cursor->GetId() <= end_id;
}

Selection &
Selection::operator++() noexcept
{
	cursor.operator++();
	SkipMismatches();
	return *this;
}
