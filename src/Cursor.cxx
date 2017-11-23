/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "Cursor.hxx"
#include "Database.hxx"
#include "Filter.hxx"

bool
Cursor::FixDeleted() noexcept
{
	if (LightCursor::FixDeleted(id)) {
		assert(!is_linked());
		id = LightCursor::operator*().GetId();
		return true;
	} else
		return false;
}

void
Cursor::Rewind() noexcept
{
	unlink();
	LightCursor::Rewind();

	if (*this)
		id = LightCursor::operator*().GetId();
}

void
Cursor::Follow() noexcept
{
	assert(append_callback);

	if (!*this && !is_linked())
		LightCursor::AddAppendListener(*this);
}

void
Cursor::OnAppend(const Record &record) noexcept
{
	assert(!is_linked());
	assert(!*this);

	SetNext(record);
	id = record.GetId();

	append_callback();
}

Cursor &
Cursor::operator++() noexcept
{
	assert(*this);

	LightCursor::operator++();
	if (*this)
		id = LightCursor::operator*().GetId();

	return *this;
}
