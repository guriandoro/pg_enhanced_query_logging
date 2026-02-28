# Bug: Parameter access bypasses `paramFetch` hook, may read uninitialized data

## Status: Fixed

## Priority: High

## Effort: Medium

## Description

The parameter formatter directly accesses `params->params[i]`:

```c
for (i = 0; i < nparams; i++)
{
    ParamExternData *prm = &params->params[i];

    if (prm->isnull || !OidIsValid(prm->ptype))
        appendStringInfoString(buf, "NULL");
    else
    {
        getTypeOutputInfo(prm->ptype, &typoutput, &typisvarlena);
        val = OidOutputFunctionCall(typoutput, prm->value);
        ...
    }
}
```

In modern PostgreSQL (11+), `ParamListInfo` may use a `paramFetch` callback
to lazily populate parameter values. When `paramFetch` is set, the
`params->params[i]` entries may be *uninitialized* until the callback is invoked.
The correct way to access parameters is through `ParamListInfoGetParam()`:

```c
prm = ParamListInfoGetParam(params, i + 1);  /* 1-based paramId */
```

Without calling the fetch hook, the code may:

1. Read `ptype == InvalidOid` for a parameter that actually has a valid type,
   causing it to be logged as `NULL` when it isn't.
2. Read an uninitialized `value` Datum, causing a crash in `OidOutputFunctionCall`.
3. Appear to work in simple cases (psql, JDBC) where parameters are pre-populated,
   but crash with prepared statements through pgbouncer, connection poolers, or
   custom client libraries that use the extended query protocol with lazy fetch.

## Location

`pg_enhanced_query_logging.c`, lines 1258-1283 (`peql_append_params`)

## Fix

Replace the direct `params->params[i]` access with the proper API. Note that
parameters are 1-based in the PostgreSQL API:

```c
for (i = 0; i < nparams; i++)
{
    ParamExternData prmdata;
    ParamExternData *prm;
    bool isnull;

    prm = params->paramFetch
        ? params->paramFetch(params, i + 1, false, &prmdata)
        : &params->params[i];

    ...
}
```

Or more simply, if the version supports it:

```c
prm = ParamListInfoGetParam(params, i + 1);
```
