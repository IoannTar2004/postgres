drop table testt;
create table testt (id serial primary key, info jsonb not null, info2 jsonb);
-- create index on testt (info, info2) ;
insert into testt (info) values ('{}');

begin;
update testt set info = jsonb_set(info, '{first}', '"val"');
create index on testt ((info->>'first'));
-- update testt set info = info #- '{1,2}' #- '{1,3}';
-- update testt set col = 40;
update testt set info = jsonb_set(info, '{second}', '"wefwef112323"');
-- update testt set info = jsonb_set(info, '{first}', '"wefwef1123"');
-- update testt set info = jsonb_set(info, '{second}', '"wefwef1123231"');
-- update testt set info = jsonb_set(info, '{first}', '"wefwef1123ed"');
-- UPDATE testt SET info = jsonb_set(info, '{first}', to_jsonb(random()::text));
-- update testt set info = jsonb_set(info, '{second}', '"wefwef11"');
-- update testt set info = jsonb_set(info, '{second}', '"wefwef112323"');
SET LOCAL enable_bitmapscan = off;

SET LOCAL enable_indexscan = on;
SET LOCAL enable_indexscan = off;

SELECT * FROM testt WHERE info->>'first' = 'val';

SET LOCAL enable_seqscan = off;
SET LOCAL enable_indexscan = on;

SELECT * FROM testt WHERE info->>'first' = 'val';

rollback;