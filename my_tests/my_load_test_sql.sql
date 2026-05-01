\timing on

drop table testt;
create table testt (id serial primary key, info jsonb not null, col int not null );
create index on testt ((info->>'first'));
insert into testt (info, col) values ('{"first": "haha"}', 10);

DO $$
    DECLARE i int;
    BEGIN
        FOR i IN 1..3000 LOOP
                UPDATE testt
                SET info = jsonb_set(jsonb_set(info, '{second}', '"val"'), '{third}', to_jsonb(i::text));
            END LOOP;
    END $$;