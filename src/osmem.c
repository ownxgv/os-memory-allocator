// SPDX-License-Identifier: BSD-3-Clause

#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "osmem.h"
#include "block_meta.h"

#define MMAP_THRESHOLD (128 * 1024)
#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define META_SIZE ALIGN(sizeof(struct block_meta))

static struct block_meta *head;
static int heap_preallocated;

/* Об'єднання сусідніх вільних блоків на купі */
static void coalesce_all(void)
{
	struct block_meta *curr = head;

	while (curr && curr->next) {
		if (curr->status == STATUS_FREE && curr->next->status == STATUS_FREE) {
			curr->size += META_SIZE + curr->next->size;
			curr->next = curr->next->next;
			if (curr->next)
				curr->next->prev = curr;
		} else {
			curr = curr->next;
		}
	}
}

/* Розбиття великого блоку на два, якщо залишок вміщує заголовок і мінімум 1 байт (8 після вирівнювання) */
static void split_block(struct block_meta *block, size_t aligned_size)
{
	if (block->size >= aligned_size + META_SIZE + ALIGNMENT) {
		struct block_meta *new_block = (struct block_meta *)((char *)block + META_SIZE + aligned_size);

		new_block->size = block->size - aligned_size - META_SIZE;
		new_block->status = STATUS_FREE;
		new_block->prev = block;
		new_block->next = block->next;

		if (block->next)
			block->next->prev = new_block;
		block->next = new_block;
		block->size = aligned_size;
	}
}

/* Пошук найкращого вільного блоку (Best Fit) */
static struct block_meta *find_best_fit(size_t aligned_size)
{
	struct block_meta *curr = head;
	struct block_meta *best = NULL;

	while (curr) {
		if (curr->status == STATUS_FREE && curr->size >= aligned_size) {
			if (!best || curr->size < best->size)
				best = curr;
		}
		curr = curr->next;
	}
	return best;
}

/* Отримання останнього блоку в списку (на купі знаходяться тільки STATUS_ALLOC та STATUS_FREE) */
static struct block_meta *get_last_heap_block(void)
{
	struct block_meta *curr = head;

	if (!curr)
		return NULL;
	while (curr->next)
		curr = curr->next;
	return curr;
}

/* Внутрішня функція виділення пам'яті з налаштовуваним порогом для malloc та calloc */
static void *os_malloc_internal(size_t size, size_t threshold)
{
	if (size == 0)
		return NULL;

	size_t aligned_size = ALIGN(size);
	size_t total_size = aligned_size + META_SIZE;

	/* Виділення через mmap, якщо загальний розмір >= порогу */
	if (total_size >= threshold) {
		void *ptr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

		DIE(ptr == MAP_FAILED, "mmap failed");
		struct block_meta *block = (struct block_meta *)ptr;

		block->size = aligned_size;
		block->status = STATUS_MAPPED;
		block->prev = NULL;
		block->next = NULL;
		return (char *)block + META_SIZE;
	}

	/* Попередня алокація купи (128 КБ) при першому зверненні до brk */
	if (!heap_preallocated) {
		void *ptr = sbrk(MMAP_THRESHOLD);

		DIE(ptr == (void *)-1, "sbrk failed");
		head = (struct block_meta *)ptr;
		head->size = MMAP_THRESHOLD - META_SIZE;
		head->status = STATUS_FREE;
		head->prev = NULL;
		head->next = NULL;
		heap_preallocated = 1;
	}

	coalesce_all();

	/* Пошук вільного блоку */
	struct block_meta *best = find_best_fit(aligned_size);

	if (best) {
		split_block(best, aligned_size);
		best->status = STATUS_ALLOC;
		return (char *)best + META_SIZE;
	}

	/* Якщо вільного блоку немає, але останній блок вільний — розширюємо його */
	struct block_meta *last = get_last_heap_block();

	if (last && last->status == STATUS_FREE) {
		size_t diff = aligned_size - last->size;
		void *ptr = sbrk(diff);

		DIE(ptr == (void *)-1, "sbrk failed");
		last->size = aligned_size;
		last->status = STATUS_ALLOC;
		return (char *)last + META_SIZE;
	}

	/* Додавання нового блоку в кінець купи */
	void *ptr = sbrk(total_size);

	DIE(ptr == (void *)-1, "sbrk failed");
	struct block_meta *new_block = (struct block_meta *)ptr;

	new_block->size = aligned_size;
	new_block->status = STATUS_ALLOC;
	new_block->prev = last;
	new_block->next = NULL;
	if (last)
		last->next = new_block;
	else
		head = new_block;

	return (char *)new_block + META_SIZE;
}

void *os_malloc(size_t size)
{
	return os_malloc_internal(size, MMAP_THRESHOLD);
}

void os_free(void *ptr)
{
	if (!ptr)
		return;

	struct block_meta *block = (struct block_meta *)((char *)ptr - META_SIZE);

	if (block->status == STATUS_MAPPED) {
		int ret = munmap(block, block->size + META_SIZE);

		DIE(ret == -1, "munmap failed");
	} else if (block->status == STATUS_ALLOC) {
		block->status = STATUS_FREE;
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return NULL;

	size_t total = nmemb * size;

	if (total / nmemb != size)
		return NULL;

	size_t page_size = getpagesize();
	void *ptr = os_malloc_internal(total, page_size);

	if (ptr)
		memset(ptr, 0, total);
	return ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	if (!ptr)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	struct block_meta *block = (struct block_meta *)((char *)ptr - META_SIZE);

	if (block->status == STATUS_FREE)
		return NULL;

	size_t aligned_size = ALIGN(size);
	size_t total_size = aligned_size + META_SIZE;

	/* Обробка блоків, виділених через mmap */
	if (block->status == STATUS_MAPPED) {
		void *new_ptr = os_malloc(size);

		if (!new_ptr)
			return NULL;
		size_t copy_size = block->size < aligned_size ? block->size : aligned_size;

		memcpy(new_ptr, ptr, copy_size);
		os_free(ptr);
		return new_ptr;
	}

	/* Якщо блок на купі просять розширити понад поріг mmap */
	if (total_size >= MMAP_THRESHOLD) {
		void *new_ptr = os_malloc(size);

		if (!new_ptr)
			return NULL;
		memcpy(new_ptr, ptr, block->size);
		os_free(ptr);
		return new_ptr;
	}

	/* Зменшення блоку на місці */
	if (block->size >= aligned_size) {
		split_block(block, aligned_size);
		return ptr;
	}

	/* Спроба розширити блок на місці: спершу зливаємо сусідні вільні блоки */
	coalesce_all();

	while (block->next && block->next->status == STATUS_FREE) {
		block->size += META_SIZE + block->next->size;
		block->next = block->next->next;
		if (block->next)
			block->next->prev = block;
		if (block->size >= aligned_size) {
			split_block(block, aligned_size);
			return ptr;
		}
	}

	/* Якщо блок опинився в кінці купи — розширюємо його через sbrk */
	if (!block->next) {
		size_t diff = aligned_size - block->size;
		void *res = sbrk(diff);

		DIE(res == (void *)-1, "sbrk failed");
		block->size = aligned_size;
		return ptr;
	}

	/* Якщо розширити на місці неможливо — виділяємо новий блок і копіюємо дані */
	void *new_ptr = os_malloc(size);

	if (!new_ptr)
		return NULL;
	memcpy(new_ptr, ptr, block->size);
	os_free(ptr);
	return new_ptr;
}