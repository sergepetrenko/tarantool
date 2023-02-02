local yaml = require('yaml')
local t = require('luatest')
local g = t.group()

local function serialize(o)
    return yaml.decode(yaml.encode(o))
end

g.test_objects_that_implement_serialize_are_aliased = function()
    local o = setmetatable({}, {
        __serialize = function() return {} end,
    })
    local arr1 = {o, o}
    local arr2 = serialize(arr1)
    t.assert_equals(arr2, arr1)
    t.assert_is_not(arr2[1], arr1[1])
    t.assert_is(arr2[1], arr2[2])
    local map1 = {a = o, b = o}
    local map2 = serialize(map1)
    t.assert_equals(map2, map1)
    t.assert_is_not(map2.a, map1.a)
    t.assert_is(map2.a, map2.b)
    local nested1 = {o, a = {{o}, b = {c = o}}}
    local nested2 = serialize(nested1)
    t.assert_equals(nested2, nested1)
    t.assert_is_not(nested2[1], nested1[1])
    t.assert_is(nested2[1], nested2.a[1][1])
    t.assert_is(nested2[1], nested2.a.b.c)
end
