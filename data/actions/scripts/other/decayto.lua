local decayItems = {
	[2041] = 2042, [2042] = 2041, [2044] = 2045, [2045] = 2044, [2047] = 2048,
	[2048] = 2047, [2050] = 2051, [2051] = 2050, [2052] = 2053, [2053] = 2052,
	[2054] = 2055, [2055] = 2054
}

function onUse(cid, item, fromPosition, target, toPosition, isHotkey)
	local decayItemId = decayItems[item.itemid]
	if not decayItemId then
		return false
	end

	local decayItem = Item(item.uid)
	decayItem:transform(decayItemId)
	decayItem:decay()

	return true
end
