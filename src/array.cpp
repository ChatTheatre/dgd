/*
 * This file is part of DGD, https://github.com/dworkin/dgd
 * Copyright (C) 1993-2010 Dworkin B.V.
 * Copyright (C) 2010-2018 DGD Authors (see the commit log for details)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

# include "dgd.h"
# include "str.h"
# include "array.h"
# include "object.h"
# include "xfloat.h"
# include "interpret.h"
# include "data.h"

static int cmp (cvoid*, cvoid*);

# define ARR_CHUNK	128

class ArrHash : public ChunkAllocated {
public:
    ArrHash(Array *a, Uint idx) {
	next = (ArrHash *) NULL;
	arr = a;
	arr->ref();
	index = idx;
    }
    ~ArrHash() {
	arr->del();
    }

    ArrHash *next;		/* next in hash table chain */
    Array *arr;			/* array entry */
    Uint index;			/* building index */
};

static class ArrHashChunk : public Chunk<ArrHash, ARR_CHUNK> {
public:
    /*
     * iterate through item and delete them
     */
    virtual bool item(ArrHash *h) {
	delete h;
	return TRUE;
    }
} hchunk;

# define MELT_CHUNK	128

class MapElt : public ChunkAllocated {
public:
    MapElt(Uint hashval, MapElt *next) {
	this->hashval = hashval;
	add = FALSE;
	idx = nil_value;
	val = nil_value;
	this->next = next;
    }
    ~MapElt() {
	if (add) {
	    i_del_value(&idx);
	    i_del_value(&val);
	}
    }

    /*
     * remove from a mapping
     */
    void remove(Dataspace *data, Array *m) {
	if (add) {
	    d_assign_elt(data, m, &idx, &nil_value);
	    d_assign_elt(data, m, &val, &nil_value);
	}
	delete this;
    }

    /*
     * clean destructed objects
     */
    bool clean(Dataspace *data, Array *m) {
	Value *v;

	switch (idx.type) {
	case T_OBJECT:
	    if (DESTRUCTED(&idx)) {
		/*
		 * index is destructed object
		 */
		if (add) {
		    d_assign_elt(data, m, &val, &nil_value);
		}
		return TRUE;
	    }
	    break;

	case T_LWOBJECT:
	    v = d_get_elts(idx.u.array);
	    if (v->type == T_OBJECT && DESTRUCTED(v)) {
		/*
		 * index is destructed object
		 */
		if (add) {
		    d_assign_elt(data, m, &idx, &nil_value);
		    d_assign_elt(data, m, &val, &nil_value);
		}
		return TRUE;
	    }
	    break;
	}
	switch (val.type) {
	case T_OBJECT:
	    if (DESTRUCTED(&val)) {
		/*
		 * value is destructed object
		 */
		if (add) {
		    d_assign_elt(data, m, &idx, &nil_value);
		}
		return TRUE;
	    }
	    break;

	case T_LWOBJECT:
	    v = d_get_elts(val.u.array);
	    if (v->type == T_OBJECT && DESTRUCTED(v)) {
		/*
		 * value is destructed object
		 */
		if (add) {
		    d_assign_elt(data, m, &idx, &nil_value);
		    d_assign_elt(data, m, &val, &nil_value);
		}
		return TRUE;
	    }
	    break;
	}

	return FALSE;
    }

    Uint hashval;		/* hash value of index */
    bool add;			/* new element? */
    Value idx;			/* index */
    Value val;			/* value */
    MapElt *next;		/* next in hash table */
};

static Chunk<MapElt, MELT_CHUNK> echunk;

# define MTABLE_SIZE	16	/* most mappings are quite small */

class MapHash : public ChunkAllocated {
public:
    MapHash() {
	size = 0;
	sizemod = 0;
	tablesize = MTABLE_SIZE;
	table = ALLOC(MapElt*, tablesize);
	memset(table, '\0', tablesize * sizeof(MapElt*));
    }
    ~MapHash() {
	unsigned short i;
	MapElt *e, *n, **t;

	for (i = size, t = table; i > 0; t++) {
	    for (e = *t; e != (MapElt *) NULL; e = n) {
		n = e->next;
		delete e;
		--i;
	    }
	}
	FREE(table);
    }

    /*
     * perform a shallow delete of this hash table
     */
    void shallowDelete()
    {
	unsigned short i;
	MapElt *e, *n, **t;

	for (i = size, t = table; i > 0; t++) {
	    for (e = *t; e != (MapElt *) NULL; e = n) {
		if (e->add) {
		    if (e->idx.type == T_STRING) {
			e->idx.u.string->del();
		    }
		    if (e->val.type == T_STRING) {
			e->val.u.string->del();
		    }
		    e->add = FALSE;
		}
		n = e->next;
		delete e;
		--i;
	    }
	}
	size = 0;
	delete this;
    }

    /*
     * extend hashtable
     */
    void grow() {
	unsigned short i;
	Uint j;
	MapElt *e, *n, **t, **newTable;

	tablesize <<= 1;
	newTable = ALLOC(MapElt*, tablesize);
	memset(newTable, '\0', tablesize * sizeof(MapElt*));

	/*
	 * copy entries from old hashtable to new hashtable
	 */
	for (i = size, t = table; i > 0; t++) {
	    for (e = *t; e != (MapElt *) NULL; e = n) {
		n = e->next;
		j = e->hashval % tablesize;
		e->next = newTable[j];
		newTable[j] = e;
		--i;
	    }
	}
	FREE(table);
	table = newTable;
    }

    /*
     * add MapElt
     */
    MapElt *add(Uint hashval) {
	Uint i;

	size++;
	i = hashval % tablesize;
	return table[i] = chunknew (echunk) MapElt(hashval, table[i]);
    }

    /*
     * remove MapElt
     */
    void remove(MapElt **p, Dataspace *data, Array *m) {
	MapElt *e;

	e = *p;
	if (e->add && --sizemod == 0) {
	    m->hashmod = FALSE;
	}
	*p = e->next;
	e->remove(data, m);
	--size;
    }

    /*
     * find MapElt in hashtable
     */
    MapElt **search(Value *val, Uint i) {
	MapElt **p, *e;

	for (p = &table[i % tablesize];
	     (e=*p) != (MapElt *) NULL; p = &e->next) {
	    if (cmp(val, &e->idx) == 0 &&
		(!T_INDEXED(val->type) || val->u.array == e->idx.u.array)) {
		return p;
	    }
	}

	return (MapElt **) NULL;
    }

    /*
     * collect MapElts from hash table
     */
    unsigned short collect(Value *v, Array *m, Dataspace *data) {
	unsigned short i, j;
	MapElt *e, **p, **t;

	t = table;
	for (i = size, size = sizemod = j = 0; i > 0; ) {
	    for (p = t++; (e=*p) != (MapElt *) NULL; --i) {
		if (m != (Array *) NULL && e->clean(data, m)) {
		    *p = e->next;
		    delete e;
		    continue;
		}

		if (e->add) {
		    e->add = FALSE;
		    *v++ = e->idx;
		    *v++ = e->val;
		    j++;
		}
		size++;

		p = &e->next;
	    }
	}
	return j;
    }

    unsigned short size;	/* # elements in hash table */
    unsigned short sizemod;	/* mapping size modification */
    Uint tablesize;		/* actual hash table size */
    MapElt **table;		/* hash table */
};

static Chunk<MapHash, ARR_CHUNK> mchunk;

# define ABCHUNKSZ	32

class ArrBak : public ChunkAllocated {
public:
    ArrBak(Array *a, Value *elts, unsigned short size, Dataplane *plane) {
	arr = a;
	original = elts;
	this->size = size;
	this->plane = plane;
    }

    /*
     * discard backup and make modifications permanent
     */
    void commit() {
	if (original != (Value *) NULL) {
	    Value *v;
	    unsigned short i;

	    for (v = original, i = size; i != 0; v++, --i) {
		i_del_value(v);
	    }
	    FREE(original);
	}
	arr->del();
    }

    /*
     * discard changes and restore backup
     */
    void discard() {
	unsigned short i;
	Value *v;

	if (arr->elts != (Value *) NULL) {
	    for (v = arr->elts, i = arr->size; i != 0; v++, --i) {
		i_del_value(v);
	    }
	    FREE(arr->elts);
	}

	if (arr->hashed != (MapHash *) NULL) {
	    delete arr->hashed;
	    arr->hashed = (MapHash *) NULL;
	    arr->hashmod = FALSE;
	}

	arr->elts = original;
	arr->size = size;
	arr->del();
    }

    Array *arr;			/* array backed up */
    unsigned short size;	/* original size (of mapping) */
    Value *original;		/* original elements */
    Dataplane *plane;		/* original dataplane */
};

class Array::Backup : public Chunk<ArrBak, ABCHUNKSZ> {
public:
    /*
     * add an array backup to the backup chunk
     */
    static void backup(Backup **ac, Array *a, Value *elts, unsigned int size,
		       Dataplane *plane) {
	ArrBak *ab;

	if (*ac == (Backup *) NULL) {
	    *ac = new Backup;
	}

	ab = chunknew (**ac) ArrBak(a, elts, size, plane);
    }

    /*
     * commit or discard when iterating over items
     */
    virtual bool item(ArrBak *ab) {
	if (plane != (Dataplane *) NULL) {
	    Backup **ac;

	    /*
	     * commit
	     */
	    ac = d_commit_arr(ab->arr, plane, ab->plane);
	    if (merge) {
		if (ac != (Backup **) NULL) {
		    /* backup on previous plane */
		    backup(ac, ab->arr, ab->original, ab->size, ab->plane);
		} else {
		    ab->commit();
		}
	    }
	} else {
	    /*
	     * discard
	     */
	    d_discard_arr(ab->arr, ab->plane);
	    ab->discard();
	}

	return TRUE;
    }

    /*
     * commit array backups
     */
    void commit(Dataplane *p, bool flag) {
	plane = p;
	merge = flag;
	items();
    }

    /*
     * discard array backups
     */
    void discard() {
	plane = (Dataplane *) NULL;
	items();
    }

private:
    Dataplane *plane;			/* plane to commit to */
    bool merge;				/* merging? */
};

static Chunk<Array, ARR_CHUNK> achunk;
static unsigned long max_size;		/* max. size of array and mapping */
static Uint atag;			/* current array tag */
static ArrHash *aht[ARRMERGETABSZ];	/* array merge table */

/*
 * initialize array handling
 */
void Array::init(unsigned int size)
{
    max_size = size;
    atag = 0;
}

Array::Array(unsigned short size)
{
    this->size = size;
    hashmod = FALSE;
    elts = (Value *) NULL;
    refCount = 0;
    odcount = 0;		/* if swapped in, check objects */
    hashed = (MapHash *) NULL;	/* only used for mappings */
}

Array::~Array()
{
    Value *v;
    unsigned short i;

    if ((v=elts) != (Value *) NULL) {
	for (i = size; i > 0; --i) {
	    i_del_value(v++);
	}
	FREE(elts);
    }

    if (hashed != (MapHash *) NULL) {
	delete hashed;
    }
}

/*
 * create a new array
 */
Array *Array::alloc(unsigned int size)
{
    return chunknew (achunk) Array(size);
}

/*
 * create a new array
 */
Array *Array::create(Dataspace *data, long size)
{
    Array *a;

    if (size > max_size) {
	error("Array too large");
    }
    a = alloc((unsigned short) size);
    if (size > 0) {
	a->elts = ALLOC(Value, size);
    }
    a->tag = atag++;
    a->odcount = Object::odcount;
    a->primary = &data->plane->alocal;
    a->prev = &data->alist;
    a->next = data->alist.next;
    a->next->prev = a;
    data->alist.next = a;
    return a;
}

/*
 * return an initialized array
 */
Array *Array::createNil(Dataspace *data, long size)
{
    int i;
    Value *v;
    Array *a;

    a = create(data, size);
    for (i = size, v = a->elts; i != 0; --i, v++) {
	*v = nil_value;
    }
    return a;
}

/*
 * Remove a reference from an array or mapping.  If none are left,
 * the array/mapping is removed.
 */
void Array::del()
{
    if (--refCount == 0) {
	static Array *dlist, *a;

	prev->next = next;
	next->prev = prev;
	prev = (Array *) NULL;
	if (dlist != (Array *) NULL) {
	    dlist->prev = this;
	    dlist = this;
	    return;
	}

	a = this;
	do {
	    dlist = a->prev;
	    delete a;
	    a = dlist;
	} while (a != (Array *) NULL);
    }
}

/*
 * free all left-over arrays in a dataspace
 */
void Array::freelist()
{
    Array *a;
    Value *v;
    unsigned short i;

    a = this;
    do {
	Array *prev;

	if ((v=a->elts) != (Value *) NULL) {
	    for (i = a->size; i > 0; --i) {
		if (v->type == T_STRING) {
		    v->u.string->del();
		}
		v++;
	    }
	    FREE(a->elts);
	    a->elts = (Value *) NULL;
	}

	if (a->hashed != (MapHash *) NULL) {
	    /*
	     * delete the hashtable of a mapping
	     */
	    a->hashed->shallowDelete();
	    a->hashed = (MapHash *) NULL;
	}

	prev = a->prev;
	delete a;
	a = prev;
    } while (a != this);
}

/*
 * free all array chunks and mapping element chunks
 */
void Array::freeall()
{
    achunk.clean();
    echunk.clean();
    mchunk.clean();
}

/*
 * prepare the array merge table
 */
void Array::merge()
{
    memset(&aht, '\0', ARRMERGETABSZ * sizeof(ArrHash *));
}

/*
 * Put an array in the merge table, and return its "index".
 */
Uint Array::put(Uint idx)
{
    ArrHash **h;

    for (h = &aht[(uintptr_t) this % ARRMERGETABSZ]; *h != (ArrHash *) NULL;
	 h = &(*h)->next) {
	if ((*h)->arr == this) {
	    return (*h)->index;
	}
    }
    /*
     * Add a new entry to the hash table.
     */
    *h = chunknew (hchunk) ArrHash(this, idx);

    return idx;
}

/*
 * clear the array merge table
 */
void Array::clear()
{
    hchunk.items();
    hchunk.clean();
}


/*
 * make a backup of the current elements of an array or mapping
 */
void Array::backup(Backup **ac)
{
    Value *v;
    unsigned short i;

# ifdef DEBUG
    if (hashmod) {
	fatal("backing up unclean mapping");
    }
# endif
    if (size != 0) {
	memcpy(v = ALLOC(Value, size), elts, size * sizeof(Value));
	for (i = size; i != 0; --i) {
	    switch (v->type) {
	    case T_STRING:
		v->u.string->ref();
		break;

	    case T_ARRAY:
	    case T_MAPPING:
	    case T_LWOBJECT:
		v->u.array->ref();
		break;
	    }
	    v++;
	}
	v -= size;
    } else {
	v = (Value *) NULL;
    }
    Backup::backup(ac, this, v, size, primary->plane);
    ref();
}

/*
 * commit current array values and discard originals
 */
void Array::commit(Backup **ac, Dataplane *plane, bool merge)
{
    if (*ac != (Backup *) NULL) {
	(*ac)->commit(plane, merge);
	if (merge) {
	    delete *ac;
	    *ac = (Backup *) NULL;
	}
    }
}

/*
 * restore originals and discard current values
 */
void Array::discard(Backup **ac)
{
    if (*ac != (Backup *) NULL) {
	(*ac)->discard();
	delete *ac;
    }
}


/*
 * NAME:	copytmp()
 * DESCRIPTION:	make temporary copies of values
 */
static void copytmp(Dataspace *data, Value *v1, Array *a)
{
    Value *v2, *o;
    unsigned short n;

    v2 = d_get_elts(a);
    if (a->odcount == Object::odcount) {
	/*
	 * no need to check for destructed objects
	 */
	memcpy(v1, v2, a->size * sizeof(Value));
    } else {
	/*
	 * Copy and check for destructed objects.  If destructed objects are
	 * found, they will be replaced by nil in the original array.
	 */
	a->odcount = Object::odcount;
	for (n = a->size; n != 0; --n) {
	    switch (v2->type) {
	    case T_OBJECT:
		if (DESTRUCTED(v2)) {
		    d_assign_elt(data, a, v2, &nil_value);
		}
		break;

	    case T_LWOBJECT:
		o = d_get_elts(v2->u.array);
		if (o->type == T_OBJECT && DESTRUCTED(o)) {
		    d_assign_elt(data, a, v2, &nil_value);
		}
		break;
	    }
	    *v1++ = *v2++;
	}
    }
}

/*
 * add two arrays
 */
Array *Array::add(Dataspace *data, Array *a2)
{
    Array *a;

    a = create(data, (long) size + a2->size);
    i_copy(a->elts, d_get_elts(this), size);
    i_copy(a->elts + size, d_get_elts(a2), a2->size);
    d_ref_imports(a);

    return a;
}

/*
 * NAME:	cmp()
 * DESCRIPTION:	compare two values
 */
static int cmp(cvoid *cv1, cvoid *cv2)
{
    Value *v1, *v2;
    int i;
    Float f1, f2;

    v1 = (Value *) cv1;
    v2 = (Value *) cv2;
    i = v1->type - v2->type;
    if (i != 0) {
	return i;	/* order by type */
    }

    switch (v1->type) {
    case T_NIL:
	return 0;

    case T_INT:
	return (v1->u.number <= v2->u.number) ?
		(v1->u.number < v2->u.number) ? -1 : 0 :
		1;

    case T_FLOAT:
	GET_FLT(v1, f1);
	GET_FLT(v2, f2);
	return f1.cmp(f2);

    case T_STRING:
	return v1->u.string->cmp(v2->u.string);

    case T_OBJECT:
	return (v1->oindex <= v2->oindex) ?
		(v1->oindex < v2->oindex) ? -1 : 0 :
		1;

    case T_ARRAY:
    case T_MAPPING:
    case T_LWOBJECT:
	return (v1->u.array->tag <= v2->u.array->tag) ?
		(v1->u.array->tag < v2->u.array->tag) ? -1 : 0 :
		1;
    }

    return 0;
}

/*
 * NAME:	search()
 * DESCRIPTION:	search for a value in an array
 */
static int search(Value *v1, Value *v2, unsigned short h, int step, bool place)
{
    unsigned short l, m;
    Int c;
    Value *v3;
    unsigned short mask;

    mask = -step;
    l = 0;
    while (l < h) {
	m = ((l + h) >> 1) & mask;
	v3 = v2 + m;
	c = cmp(v1, v3);
	if (c == 0) {
	    if (T_INDEXED(v1->type) && v1->u.array != v3->u.array) {
		/*
		 * It is possible for one object to export an array, both
		 * objects being swapped out after that, and the other object
		 * exporting the array back again.  This gives two arrays with
		 * identical tags, which do not point to the same actual values
		 * and are not guaranteed to contain the same values, either.
		 * A possible way out is to give new tags to imported arrays
		 * and to resort all mappings before swapping them out.
		 * The solution used here is to check every array with this tag,
		 * and hope this kind of thing doesn't occur too often...
		 */
		/* search forward */
		for (;;) {
		    m += step;
		    v3 += step;
		    if (m == h || !T_INDEXED(v3->type)) {
			break;	/* out of range */
		    }
		    if (v1->u.array == v3->u.array) {
			return m;	/* found the right one */
		    }
		    if (v1->u.array->tag != v3->u.array->tag) {
			break;		/* wrong tag */
		    }
		}
		/* search backward */
		m = ((l + h) >> 1) & mask;
		v3 = v2 + m;
		for (;;) {
		    v3 -= step;
		    if (m == l || !T_INDEXED(v3->type)) {
			break;	/* out of range */
		    }
		    m -= step;
		    if (v1->u.array == v3->u.array) {
			return m;	/* found the right one */
		    }
		    if (v1->u.array->tag != v3->u.array->tag) {
			break;		/* wrong tag */
		    }
		}
		break;		/* not found */
	    }
	    return m;		/* found */
	} else if (c < 0) {
	    h = m;		/* search in lower half */
	} else {
	    l = (m + step);	/* search in upper half */
	}
    }
    /*
     * not found
     */
    return (place) ? l : -1;
}

/*
 * subtract one array from another
 */
Array *Array::sub(Dataspace *data, Array *a2)
{
    Value *v1, *v2, *v3, *o;
    Array *a3;
    unsigned short n;

    if (a2->size == 0) {
	/*
	 * array - ({ })
	 * Return a copy of the first array.
	 */
	a3 = create(data, size);
	i_copy(a3->elts, d_get_elts(this), size);
	d_ref_imports(a3);
	return a3;
    }

    /* create new array */
    a3 = create(data, size);
    if (a3->size == 0) {
	/* subtract from empty array */
	return a3;
    }

    /* copy and sort values of subtrahend */
    copytmp(data, v2 = ALLOCA(Value, a2->size), a2);
    qsort(v2, a2->size, sizeof(Value), cmp);

    v1 = d_get_elts(this);
    v3 = a3->elts;
    if (odcount == Object::odcount) {
	for (n = size; n > 0; --n) {
	    if (search(v1, v2, a2->size, 1, FALSE) < 0) {
		/*
		 * not found in subtrahend: copy to result array
		 */
		i_ref_value(v1);
		*v3++ = *v1;
	    }
	    v1++;
	}
    } else {
	odcount = Object::odcount;
	for (n = size; n > 0; --n) {
	    switch (v1->type) {
	    case T_OBJECT:
		if (DESTRUCTED(v1)) {
		    /* replace destructed object by nil */
		    d_assign_elt(primary->data, this, v1, &nil_value);
		}
		break;

	    case T_LWOBJECT:
		o = d_get_elts(v1->u.array);
		if (o->type == T_OBJECT && DESTRUCTED(o)) {
		    /* replace destructed object by nil */
		    d_assign_elt(primary->data, this, v1, &nil_value);
		}
		break;
	    }
	    if (search(v1, v2, a2->size, 1, FALSE) < 0) {
		/*
		 * not found in subtrahend: copy to result array
		 */
		i_ref_value(v1);
		*v3++ = *v1;
	    }
	    v1++;
	}
    }
    AFREE(v2);	/* free copy of values of subtrahend */

    a3->size = v3 - a3->elts;
    if (a3->size == 0) {
	FREE(a3->elts);
	a3->elts = (Value *) NULL;
    }

    d_ref_imports(a3);
    return a3;
}

/*
 * A - (A - B).  If A and B are sets, the result is a set also.
 */
Array *Array::intersect(Dataspace *data, Array *a2)
{
    Value *v1, *v2, *v3, *o;
    Array *a3;
    unsigned short n;

    if (size == 0 || a2->size == 0) {
	/* array & ({ }) */
	return create(data, 0);
    }

    /* create new array */
    a3 = create(data, size);

    /* copy and sort values of 2nd array */
    copytmp(data, v2 = ALLOCA(Value, a2->size), a2);
    qsort(v2, a2->size, sizeof(Value), cmp);

    v1 = d_get_elts(this);
    v3 = a3->elts;
    if (odcount == Object::odcount) {
	for (n = size; n > 0; --n) {
	    if (search(v1, v2, a2->size, 1, FALSE) >= 0) {
		/*
		 * element is in both arrays: copy to result array
		 */
		i_ref_value(v1);
		*v3++ = *v1;
	    }
	    v1++;
	}
    } else {
	odcount = Object::odcount;
	for (n = size; n > 0; --n) {
	    switch (v1->type) {
	    case T_OBJECT:
		if (DESTRUCTED(v1)) {
		    /* replace destructed object by nil */
		    d_assign_elt(primary->data, this, v1, &nil_value);
		}
		break;

	    case T_LWOBJECT:
		o = d_get_elts(v1->u.array);
		if (o->type == T_OBJECT && DESTRUCTED(o)) {
		    /* replace destructed object by nil */
		    d_assign_elt(primary->data, this, v1, &nil_value);
		}
		break;
	    }
	    if (search(v1, v2, a2->size, 1, FALSE) >= 0) {
		/*
		 * element is in both arrays: copy to result array
		 */
		i_ref_value(v1);
		*v3++ = *v1;
	    }
	    v1++;
	}
    }
    AFREE(v2);	/* free copy of values of 2nd array */

    a3->size = v3 - a3->elts;
    if (a3->size == 0) {
	FREE(a3->elts);
	a3->elts = (Value *) NULL;
    }

    d_ref_imports(a3);
    return a3;
}

/*
 * A + (B - A).  If A and B are sets, the result is a set also.
 */
Array *Array::setadd(Dataspace *data, Array *a2)
{
    Value *v, *v1, *v2, *o;
    Value *v3;
    Array *a3;
    unsigned short n;

    if (size == 0) {
	/* ({ }) | array */
	a3 = create(data, a2->size);
	i_copy(a3->elts, d_get_elts(a2), a2->size);
	d_ref_imports(a3);
	return a3;
    }
    if (a2->size == 0) {
	/* array | ({ }) */
	a3 = create(data, size);
	i_copy(a3->elts, d_get_elts(this), size);
	d_ref_imports(a3);
	return a3;
    }

    /* make room for elements to add */
    v3 = ALLOCA(Value, a2->size);

    /* copy and sort values of 1st array */
    copytmp(data, v1 = ALLOCA(Value, size), this);
    qsort(v1, size, sizeof(Value), cmp);

    v = v3;
    v2 = d_get_elts(a2);
    if (a2->odcount == Object::odcount) {
	for (n = a2->size; n > 0; --n) {
	    if (search(v2, v1, size, 1, FALSE) < 0) {
		/*
		 * element is only in second array: copy to result array
		 */
		*v++ = *v2;
	    }
	    v2++;
	}
    } else {
	a2->odcount = Object::odcount;
	for (n = a2->size; n > 0; --n) {
	    switch (v2->type) {
	    case T_OBJECT:
		if (DESTRUCTED(v2)) {
		    /* replace destructed object by nil */
		    d_assign_elt(a2->primary->data, a2, v2, &nil_value);
		}
		break;

	    case T_LWOBJECT:
		o = d_get_elts(v2->u.array);
		if (o->type == T_OBJECT && DESTRUCTED(o)) {
		    /* replace destructed object by nil */
		    d_assign_elt(a2->primary->data, a2, v2, &nil_value);
		}
		break;
	    }
	    if (search(v2, v1, size, 1, FALSE) < 0) {
		/*
		 * element is only in second array: copy to result array
		 */
		*v++ = *v2;
	    }
	    v2++;
	}
    }
    AFREE(v1);	/* free copy of values of 1st array */

    n = v - v3;
    if ((long) size + n > max_size) {
	AFREE(v3);
	error("Array too large");
    }

    a3 = create(data, (long) size + n);
    i_copy(a3->elts, elts, size);
    i_copy(a3->elts + size, v3, n);
    AFREE(v3);

    d_ref_imports(a3);
    return a3;
}

/*
 * (A - B) + (B - A).  If A and B are sets, the result is a set	also.
 */
Array *Array::setxadd(Dataspace *data, Array *a2)
{
    Value *v, *w, *v1, *v2;
    Value *v3;
    Array *a3;
    unsigned short n, sz;
    unsigned short num;

    if (size == 0) {
	/* ({ }) ^ array */
	a3 = create(data, a2->size);
	i_copy(a3->elts, d_get_elts(a2), a2->size);
	d_ref_imports(a3);
	return a3;
    }
    if (a2->size == 0) {
	/* array ^ ({ }) */
	a3 = create(data, size);
	i_copy(a3->elts, d_get_elts(this), size);
	d_ref_imports(a3);
	return a3;
    }

    /* copy values of 1st array */
    copytmp(data, v1 = ALLOCA(Value, size), this);

    /* copy and sort values of 2nd array */
    copytmp(data, v2 = ALLOCA(Value, a2->size), a2);
    qsort(v2, a2->size, sizeof(Value), cmp);

    /* room for first half of result */
    v3 = ALLOCA(Value, size);

    v = v3;
    w = v1;
    for (n = size; n > 0; --n) {
	if (search(v1, v2, a2->size, 1, FALSE) < 0) {
	    /*
	     * element is only in first array: copy to result array
	     */
	    *v++ = *v1;
	} else {
	    /*
	     * element is in both: keep it for the next round
	     */
	    *w++ = *v1;
	}
	v1++;
    }
    num = v - v3;

    /* sort copy of 1st array */
    v1 -= size;
    qsort(v1, sz = w - v1, sizeof(Value), cmp);

    v = v2;
    w = a2->elts;
    for (n = a2->size; n > 0; --n) {
	if (search(w, v1, sz, 1, FALSE) < 0) {
	    /*
	     * element is only in second array: copy to 2nd result array
	     */
	    *v++ = *w;
	}
	w++;
    }

    n = v - v2;
    if ((long) num + n > max_size) {
	AFREE(v3);
	AFREE(v2);
	AFREE(v1);
	error("Array too large");
    }

    a3 = create(data, (long) num + n);
    i_copy(a3->elts, v3, num);
    i_copy(a3->elts + num, v2, n);
    AFREE(v3);
    AFREE(v2);
    AFREE(v1);

    d_ref_imports(a3);
    return a3;
}

/*
 * index an array
 */
unsigned short Array::index(long l)
{
    if (l < 0 || l >= (long) size) {
	error("Array index out of range");
    }
    return l;
}

/*
 * check an array subrange
 */
void Array::checkRange(long l1, long l2)
{
    if (l1 < 0 || l1 > l2 + 1 || l2 >= (long) size) {
	error("Invalid array range");
    }
}

/*
 * return a subrange of an array
 */
Array *Array::range(Dataspace *data, long l1, long l2)
{
    Array *range;

    if (l1 < 0 || l1 > l2 + 1 || l2 >= (long) size) {
	error("Invalid array range");
    }

    range = create(data, l2 - l1 + 1);
    i_copy(range->elts, d_get_elts(this) + l1, (unsigned short) (l2 - l1 + 1));
    d_ref_imports(range);
    return range;
}


/*
 * create a new mapping
 */
Array *Array::mapCreate(Dataspace *data, long size)
{
    Array *m;

    if (size > max_size << 1) {
	error("Mapping too large");
    }
    m = Array::alloc((unsigned short) size);
    if (size > 0) {
	m->elts = ALLOC(Value, size);
    }
    m->tag = atag++;
    m->odcount = Object::odcount;
    m->primary = &data->plane->alocal;
    m->prev = &data->alist;
    m->next = data->alist.next;
    m->next->prev = m;
    data->alist.next = m;
    return m;
}

/*
 * prune and sort a mapping
 */
void Array::mapSort()
{
    unsigned short i, sz;
    Value *v, *w;

    for (i = size, sz = 0, v = w = elts; i > 0; i -= 2) {
	if (!VAL_NIL(v + 1)) {
	    *w++ = *v++;
	    *w++ = *v++;
	    sz += 2;
	} else {
	    /* delete index and skip zero value */
	    i_del_value(v);
	    v += 2;
	}
    }

    if (sz != 0) {
	qsort(v = elts, i = sz >> 1, 2 * sizeof(Value), cmp);
	while (--i != 0) {
	    if (cmp((cvoid *) v, (cvoid *) &v[2]) == 0 &&
		(!T_INDEXED(v->type) || v->u.array == v[2].u.array)) {
		error("Identical indices in mapping");
	    }
	    v += 2;
	}
    } else if (size > 0) {
	FREE(elts);
	elts = (Value *) NULL;
    }
    size = sz;
}

/*
 * commit changes from the hash table to the array part
 */
void Array::mapDehash(Dataspace *data, bool clean)
{
    unsigned short sz, i, j;
    Value *v1, *v2, *v3;
    MapElt *e, **t, **p;

    if (clean && size != 0) {
	/*
	 * remove destructed objects from array part
	 */
	sz = 0;
	v1 = v2 = d_get_elts(this);
	for (i = size; i > 0; i -= 2) {
	    switch (v2->type) {
	    case T_OBJECT:
		if (DESTRUCTED(v2)) {
		    /*
		     * index is destructed object
		     */
		    d_assign_elt(data, this, v2 + 1, &nil_value);
		    v2 += 2;
		    continue;
		}
		break;

	    case T_LWOBJECT:
		v3 = d_get_elts(v2->u.array);
		if (v3->type == T_OBJECT && DESTRUCTED(v3)) {
		    /*
		     * index is destructed object
		     */
		    d_assign_elt(data, this, v2++, &nil_value);
		    d_assign_elt(data, this, v2++, &nil_value);
		    continue;
		}
		break;
	    }
	    switch (v2[1].type) {
	    case T_OBJECT:
		if (DESTRUCTED(&v2[1])) {
		    /*
		     * value is destructed object
		     */
		    d_assign_elt(data, this, v2, &nil_value);
		    v2 += 2;
		    continue;
		}
		break;

	    case T_LWOBJECT:
		v3 = d_get_elts(v2[1].u.array);
		if (v3->type == T_OBJECT && DESTRUCTED(v3)) {
		    /*
		     * value is destructed object
		     */
		    d_assign_elt(data, this, v2++, &nil_value);
		    d_assign_elt(data, this, v2++, &nil_value);
		    continue;
		}
		break;
	    }

	    *v1++ = *v2++;
	    *v1++ = *v2++;
	    sz += 2;
	}

	if (sz != size) {
	    d_change_map(this);
	    size = sz;
	    if (sz == 0) {
		FREE(elts);
		elts = (Value *) NULL;
	    }
	}
    }

    if (hashmod || (clean && hashed != (MapHash *) NULL && hashed->size != 0)) {
	/*
	 * merge copy of hashtable with sorted array
	 */
	j = hashed->size;
	v2 = ALLOCA(Value, j << 1);
	sz = hashed->collect(v2, (clean) ? this : (Array *) NULL, data);

	if (j != hashed->size) {
	    d_change_map(this);
	}
	hashmod = FALSE;

	if (sz != 0) {
	    qsort(v2, sz, sizeof(Value) << 1, cmp);
	    sz <<= 1;

	    /*
	     * merge the two value arrays
	     */
	    v1 = elts;
	    v3 = ALLOC(Value, size + sz);
	    for (i = size, j = sz; i > 0 && j > 0; ) {
		if (cmp(v1, v2) <= 0) {
		    *v3++ = *v1++;
		    *v3++ = *v1++;
		    i -= 2;
		} else {
		    *v3++ = *v2++;
		    *v3++ = *v2++;
		    j -= 2;
		}
	    }

	    /*
	     * copy tails of arrays
	     */
	    memcpy(v3, v1, i * sizeof(Value));
	    v3 += i;
	    memcpy(v3, v2, j * sizeof(Value));
	    v3 += j;

	    v2 -= (sz - j);
	    if (size > 0) {
		FREE(elts);
	    }
	    size += sz;
	    elts = v3 - size;
	}

	AFREE(v2);
    }
}

/*
 * delete hash table of mapping
 */
void Array::mapRemoveHash()
{
    if (hashed != (MapHash *) NULL) {
	if (hashmod) {
	    mapDehash(primary->data, FALSE);
	}
	delete hashed;
	hashed = (MapHash *) NULL;
    }
}

/*
 * compact a mapping: copy new elements from the hash table into the array,
 * and remove destructed objects
 */
void Array::mapCompact(Dataspace *data)
{
    if (hashmod || odcount != Object::odcount) {
	if (hashmod && (!THISPLANE(primary) || !SAMEPLANE(data, primary->data)))
	{
	    mapDehash(data, FALSE);
	}

	mapDehash(data, TRUE);
	odcount = Object::odcount;
    }
}

/*
 * return the size of a mapping
 */
unsigned short Array::mapSize(Dataspace *data)
{
    mapCompact(data);
    return size >> 1;
}

/*
 * add two mappings
 */
Array *Array::mapAdd(Dataspace *data, Array *m2)
{
    Value *v1, *v2, *v3;
    unsigned short n1, n2;
    Int c;
    Array *m3;

    mapCompact(data);
    m2->mapCompact(data);
    m3 = mapCreate(data, (long) size + m2->size);
    if (m3->size == 0) {
	/* add two empty mappings */
	return m3;
    }

    v1 = elts;
    v2 = m2->elts;
    v3 = m3->elts;
    for (n1 = size, n2 = m2->size; n1 > 0 && n2 > 0; ) {
	c = cmp(v1, v2);
	if (c < 0) {
	    /* the smaller element is in mapping */
	    i_copy(v3, v1, 2);
	    v1 += 2; v3 += 2; n1 -= 2;
	} else {
	    /* the smaller - or overriding - element is in m2 */
	    i_copy(v3, v2, 2);
	    v3 += 2;
	    if (c == 0) {
		/* equal elements? */
		if (T_INDEXED(v1->type) && v1->u.array != v2->u.array) {
		    Value *v;
		    unsigned short n;

		    /*
		     * The array tags are the same, but the arrays are not.
		     * Check ahead to see if the array is somewhere else
		     * in m2; if not, copy the element from mapping as well.
		     */
		    v = v2; n = n2;
		    for (;;) {
			v += 2; n -= 2;
			if (n == 0 || !T_INDEXED(v->type) ||
			    v->u.array->tag != v1->u.array->tag) {
			    /* not in m2 */
			    i_copy(v3, v1, 2);
			    v3 += 2;
			    break;
			}
			if (v->u.array == v1->u.array) {
			    /* also in m2 */
			    break;
			}
		    }
		}
		/* skip mapping */
		v1 += 2; n1 -= 2;
	    }
	    v2 += 2; n2 -= 2;
	}
    }

    /* copy tail part of mapping */
    i_copy(v3, v1, n1);
    v3 += n1;
    /* copy tail part of m2 */
    i_copy(v3, v2, n2);
    v3 += n2;

    m3->size = v3 - m3->elts;
    if (m3->size == 0) {
	FREE(m3->elts);
	m3->elts = (Value *) NULL;
    }

    d_ref_imports(m3);
    return m3;
}

/*
 * NAME:	mapping->sub()
 * DESCRIPTION:	subtract an array from a mapping
 */
Array *Array::mapSub(Dataspace *data, Array *a2)
{
    Value *v1, *v2, *v3;
    unsigned short n1, n2;
    Int c;
    Array *m3;

    mapCompact(data);
    m3 = mapCreate(data, size);
    if (size == 0) {
	/* subtract from empty mapping */
	return m3;
    }
    if (a2->size == 0) {
	/* subtract empty array */
	i_copy(m3->elts, elts, size);
	d_ref_imports(m3);
	return m3;
    }

    /* copy and sort values of array */
    copytmp(data, v2 = ALLOCA(Value, a2->size), a2);
    qsort(v2, a2->size, sizeof(Value), cmp);

    v1 = elts;
    v3 = m3->elts;
    for (n1 = size, n2 = a2->size; n1 > 0 && n2 > 0; ) {
	c = cmp(v1, v2);
	if (c < 0) {
	    /* the smaller element is in mapping */
	    i_copy(v3, v1, 2);
	    v1 += 2; v3 += 2; n1 -= 2;
	} else if (c > 0) {
	    /* the smaller element is in a2 */
	    v2++; --n2;
	} else {
	    /* equal elements? */
	    if (T_INDEXED(v1->type) && v1->u.array != v2->u.array) {
		Value *v;
		unsigned short n;

		/*
		 * The array tags are the same, but the arrays are not.
		 * Check ahead to see if the array is somewhere else
		 * in a2; if not, copy the element from mapping.
		 */
		v = v2; n = n2;
		for (;;) {
		    v++; --n;
		    if (n == 0 || !T_INDEXED(v->type) ||
			v->u.array->tag != v1->u.array->tag) {
			/* not in a2 */
			i_copy(v3, v1, 2);
			v3 += 2;
			break;
		    }
		    if (v->u.array == v1->u.array) {
			/* also in a2 */
			break;
		    }
		}
	    }
	    /* skip mapping */
	    v1 += 2; n1 -= 2;
	}
    }
    AFREE(v2 - (a2->size - n2));

    /* copy tail part of mapping */
    i_copy(v3, v1, n1);
    v3 += n1;

    m3->size = v3 - m3->elts;
    if (m3->size == 0) {
	FREE(m3->elts);
	m3->elts = (Value *) NULL;
    }

    d_ref_imports(m3);
    return m3;
}

/*
 * intersect a mapping with an array
 */
Array *Array::mapIntersect(Dataspace *data, Array *a2)
{
    Value *v1, *v2, *v3;
    unsigned short n1, n2;
    Int c;
    Array *m3;

    mapCompact(data);
    if (a2->size == 0) {
	/* intersect with empty array */
	return mapCreate(data, 0);
    }
    m3 = mapCreate(data, size);
    if (size == 0) {
	/* intersect with empty mapping */
	return m3;
    }

    /* copy and sort values of array */
    copytmp(data, v2 = ALLOCA(Value, a2->size), a2);
    qsort(v2, a2->size, sizeof(Value), cmp);

    v1 = elts;
    v3 = m3->elts;
    for (n1 = size, n2 = a2->size; n1 > 0 && n2 > 0; ) {
	c = cmp(v1, v2);
	if (c < 0) {
	    /* the smaller element is in mapping */
	    v1 += 2; n1 -= 2;
	} else if (c > 0) {
	    /* the smaller element is in a2 */
	    v2++; --n2;
	} else {
	    /* equal elements? */
	    if (T_INDEXED(v1->type) && v1->u.array != v2->u.array) {
		Value *v;
		unsigned short n;

		/*
		 * The array tags are the same, but the arrays are not.
		 * Check ahead to see if the array is somewhere else
		 * in a2; if not, don't copy the element from mapping.
		 */
		v = v2; n = n2;
		for (;;) {
		    v++; --n;
		    if (n == 0 || !T_INDEXED(v->type) ||
			v->u.array->tag != v1->u.array->tag) {
			/* not in a2 */
			break;
		    }
		    if (v->u.array == v1->u.array) {
			/* also in a2 */
			i_copy(v3, v1, 2);
			v3 += 2; v1 += 2; n1 -= 2;
			break;
		    }
		}
	    } else {
		/* equal */
		i_copy(v3, v1, 2);
		v3 += 2; v1 += 2; n1 -= 2;
	    }
	    v2++; --n2;
	}
    }
    AFREE(v2 - (a2->size - n2));

    m3->size = v3 - m3->elts;
    if (m3->size == 0) {
	FREE(m3->elts);
	m3->elts = (Value *) NULL;
    }

    d_ref_imports(m3);
    return m3;
}

/*
 * Index a mapping with a value. If a third argument is supplied, perform an
 * assignment; otherwise return the indexed value.
 */
Value *Array::mapIndex(Dataspace *data, Value *val, Value *elt, Value *verify)
{
    Uint i;
    MapElt *e, **p;
    bool del, add, hash;

    i = 0;

    if (elt != (Value *) NULL && VAL_NIL(elt)) {
	elt = (Value *) NULL;
	del = TRUE;
    } else {
	del = FALSE;
    }

    if (hashmod && (!THISPLANE(primary) || !SAMEPLANE(data, primary->data))) {
	mapDehash(data, FALSE);
    }

    switch (val->type) {
    case T_NIL:
	i = 4747;
	break;

    case T_INT:
	i = val->u.number;
	break;

    case T_FLOAT:
	i = VFLT_HASH(val);
	break;

    case T_STRING:
	i = Hashtab::hashstr(val->u.string->text, STRMAPHASHSZ) ^
							    val->u.string->len;
	break;

    case T_OBJECT:
	i = val->oindex;
	break;

    case T_ARRAY:
    case T_MAPPING:
    case T_LWOBJECT:
	i = (unsigned short) ((uintptr_t) val->u.array >> 3);
	break;
    }

    hash = FALSE;
    if (hashed != (MapHash *) NULL) {
	p = hashed->search(val, i);
	if (p != (MapElt **) NULL) {
	    /*
	     * found in the hashtable
	     */
	    hash = TRUE;
	    e = *p;
	    if (elt != (Value *) NULL &&
		(verify == (Value *) NULL ||
		 (e->val.type == T_STRING &&
		  e->val.u.string == verify->u.string))) {
		/*
		 * change element
		 */
		if (val->type == T_OBJECT) {
		    e->idx.u.objcnt = val->u.objcnt;	/* refresh */
		}
		if (e->add) {
		    d_assign_elt(data, this, &e->val, elt);
		    return &e->val;
		} else {
		    /* "real" assignment later in array part */
		    e->val = *elt;
		}
	    } else if (del ||
		       (val->type == T_OBJECT &&
			val->u.objcnt != e->idx.u.objcnt)) {
		/*
		 * delete element
		 */
		add = e->add;
		hashed->remove(p, data, this);

		if (add) {
		    return &nil_value;
		}

		/* change array part also */
	    } else {
		/*
		 * not changed or deleted
		 */
		return &e->val;
	    }
	}
    }

    add = TRUE;
    if (size > 0) {
	int n;
	Value *v;

	n = search(val, d_get_elts(this), size, 2, FALSE);
	if (n >= 0) {
	    /*
	     * found in the array
	     */
	    v = &elts[n];
	    if (elt != (Value *) NULL &&
		(verify == (Value *) NULL ||
		 (v[1].type == T_STRING && v[1].u.string == verify->u.string)))
	    {
		/*
		 * change the element
		 */
		d_assign_elt(data, this, v + 1, elt);
		if (val->type == T_OBJECT) {
		    v->modified = TRUE;
		    v->u.objcnt = val->u.objcnt;	/* refresh */
		}
	    } else if (del ||
		       (val->type == T_OBJECT &&
			val->u.objcnt != v->u.objcnt)) {
		/*
		 * delete the element
		 */
		d_assign_elt(data, this, v, &nil_value);
		d_assign_elt(data, this, v + 1, &nil_value);

		size -= 2;
		if (size == 0) {
		    /* last element removed */
		    FREE(elts);
		    elts = (Value *) NULL;
		} else {
		    /* move tail */
		    memmove(v, v + 2, (size - n) * sizeof(Value));
		}
		d_change_map(this);
		return &nil_value;
	    }
	    val = v;
	    elt = v + 1;
	    add = FALSE;
	}
    }

    if (elt == (Value *) NULL) {
	return &nil_value;	/* not found */
    }

    if (!hash) {
	/*
	 * extend mapping
	 */
	if (add &&
	    (size >> 1) + ((hashed == (MapHash *) NULL) ?
					    0 : hashed->sizemod) >= max_size) {
	    mapCompact(data);
	    if (size >> 1 >= max_size) {
		error("Mapping too large to grow");
	    }
	}

	if (hashed == (MapHash *) NULL) {
	    /*
	     * add hash table to this mapping
	     */
	    hashed = chunknew (mchunk) MapHash;
	} else if (size << 2 >= hashed->tablesize * 3) {
	    /*
	     * extend hash table for this mapping
	     */
	    hashed->grow();
	}
	e = hashed->add(i);

	if (add) {
	    e->add = TRUE;
	    d_assign_elt(data, this, &e->idx, val);
	    d_assign_elt(data, this, &e->val, elt);
	    hashed->sizemod++;
	    hashmod = TRUE;
	    d_change_map(this);
	} else {
	    e->idx = *val;
	    e->val = *elt;
	}
    }

    return elt;
}

/*
 * return a mapping value subrange
 */
Array *Array::mapRange(Dataspace *data, Value *v1, Value *v2)
{
    unsigned short from, to;
    Array *range;

    mapCompact(data);

    /* determine subrange */
    from = (v1 == (Value *) NULL) ? 0 : search(v1, elts, size, 2, TRUE);
    if (v2 == (Value *) NULL) {
	to = size;
    } else {
	to = search(v2, elts, size, 2, TRUE);
	if (to < size && cmp(v2, &elts[to]) == 0 &&
	    (!T_INDEXED(v2->type) || v2->u.array == elts[to].u.array)) {
	    /*
	     * include last element
	     */
	    to += 2;
	}
    }
    if (from >= to) {
	return mapCreate(data, 0);	/* empty subrange */
    }

    /* copy subrange */
    range = mapCreate(data, to -= from);
    i_copy(range->elts, elts + from, to);

    d_ref_imports(range);
    return range;
}

/*
 * return the indices of a mapping
 */
Array *Array::mapIndices(Dataspace *data)
{
    Array *indices;
    Value *v1, *v2;
    unsigned short n;

    mapCompact(data);
    indices = create(data, n = size >> 1);
    v1 = indices->elts;
    for (v2 = elts; n > 0; v2 += 2, --n) {
	i_ref_value(v2);
	*v1++ = *v2;
    }

    d_ref_imports(indices);
    return indices;
}

/*
 * return the values of a mapping
 */
Array *Array::mapValues(Dataspace *data)
{
    Array *values;
    Value *v1, *v2;
    unsigned short n;

    mapCompact(data);
    values = create(data, n = size >> 1);
    v1 = values->elts;
    for (v2 = elts + 1; n > 0; v2 += 2, --n) {
	i_ref_value(v2);
	*v1++ = *v2;
    }

    d_ref_imports(values);
    return values;
}


/*
 * create a new light-weight object
 */
Array *Array::lwoCreate(Dataspace *data, Object *obj)
{
    Control *ctrl;
    Array *a;
    Float flt;

    obj->lightWeight();
    ctrl = obj->control();
    a = alloc(ctrl->nvariables + 2);
    a->elts = ALLOC(Value, ctrl->nvariables + 2);
    PUT_OBJVAL(&a->elts[0], obj);
    flt.high = FALSE;
    flt.low = obj->update;
    PUT_FLTVAL(&a->elts[1], flt);
    d_new_variables(ctrl, a->elts + 2);
    a->tag = atag++;
    a->odcount = Object::odcount;
    a->primary = &data->plane->alocal;
    a->prev = &data->alist;
    a->next = data->alist.next;
    a->next->prev = a;
    data->alist.next = a;
    return a;
}

/*
 * copy a light-weight object
 */
Array *Array::lwoCopy(Dataspace *data)
{
    Array *copy;

    copy = alloc(size);
    i_copy(copy->elts = ALLOC(Value, size), elts, size);
    copy->tag = atag++;
    copy->odcount = Object::odcount;
    copy->primary = &data->plane->alocal;
    copy->prev = &data->alist;
    copy->next = data->alist.next;
    copy->next->prev = copy;
    data->alist.next = copy;
    d_ref_imports(copy);
    return copy;
}
