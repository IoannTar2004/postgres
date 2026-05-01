drop table testt;
create table testt (id serial primary key, info jsonb not null, info2 jsonb);
create index on testt ((info->>'1'));
insert into testt (info) values ('{"1": "val"}');