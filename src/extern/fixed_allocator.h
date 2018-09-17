#ifndef FIXED_ALLOCATOR_INCLUDED
#define FIXED_ALLOCATOR_INCLUDED

#include <stdexcept>
#include <algorithm>
#include <functional>
#include <type_traits>

//**********************************************************************
// fixed_allocator
//**********************************************************************
template <typename T, const size_t MAX_SIZE>
class fixed_allocator
{
private:

  static const bool FREE   = false;
  static const bool IN_USE = true;

public:

  typedef T                  value_type;
  typedef value_type *       pointer;
  typedef const value_type * const_pointer;
  typedef value_type &       reference;
  typedef const value_type &  const_reference;
  typedef std::size_t        size_type;
  typedef std::ptrdiff_t     difference_type;

  //*********************************************************************
  // rebind
  //*********************************************************************
  template<typename U>
  struct rebind
  {
      typedef fixed_allocator<U, MAX_SIZE>
      other;
  };

  //*********************************************************************
  // Constructor
  //*********************************************************************
  fixed_allocator()
    : p_first_free(in_use)
  {
    initialise();
  }

  //*********************************************************************
  // Copy constructor
  //*********************************************************************
  fixed_allocator(const fixed_allocator &rhs)
    : p_first_free(in_use)
  {
    initialise();
  }

  //*********************************************************************
  // Templated copy constructor
  //*********************************************************************
  template<typename U>
  fixed_allocator(const fixed_allocator<U, MAX_SIZE>&rhs)
      : p_first_free(in_use)
  {
    initialise();
  }

  //*********************************************************************
  // Destructor
  //*********************************************************************
  ~fixed_allocator()
  {
  }

  //*********************************************************************
  // address
  //*********************************************************************
  pointer address(reference x) const
  {
    return (&x);
  }

  //*********************************************************************
  // address
  //*********************************************************************
  const_pointer address(const_reference x) const
  {
    return (x);
  }

  //*********************************************************************
  // allocate
  // Allocates from the internal array.
  //*********************************************************************
  pointer allocate(size_type n, const_pointer cp = 0)
  {
    // Pointers to the 'in_use' flags.
    bool *p_first     = p_first_free;
    bool *const p_end = &in_use[MAX_SIZE];

    // 'Find first fit' allocation algorithm, starting from the first free slot.
    // If n == 1 then we already have the free slot address or p_end.
     if (n == 1)
    {
      // No space left?
      if (p_first == p_end)
      {
        throw std::bad_alloc();
      }

      // Mark the slot as 'in use'
      *p_first = IN_USE;
    }
    else
    {
      // Search for a big enough range of free slots.
      p_first = std::search_n(p_first, p_end, static_cast<long>(n), FREE);

      // Not enough space found?
      if (p_first == p_end)
      {
        throw std::bad_alloc();
      }

      // Mark the range as 'in use'
      std::fill(p_first, p_first + n, IN_USE);
    }

    // Update the 'first free' pointer if necessary.
    if (p_first == p_first_free)
    {
      // Find the next free slot or p_end
      p_first_free = std::find(p_first + n, p_end, FREE);
    }

    // Return the memory allocation.
    const size_t offset = std::distance(in_use, p_first) * sizeof(value_type);

    return (reinterpret_cast<pointer>(&p_buffer[offset]));
  }

  //*********************************************************************
  // deallocate
  // Clears the 'in_use' flags for the deallocated items.
  //*********************************************************************
  void deallocate(pointer p, size_type n)
  {
    // Find the start of the range.
    size_t index = std::distance(p_buffer, reinterpret_cast<char *>(p)) / sizeof(value_type);

    bool *p_first = &in_use[index];

    // Mark the range as 'free'.
    if (n == 1)
    {
      *p_first = FREE;
    }
    else
    {
      std::fill(p_first, p_first + n, FREE);
    }

    // Update the 'first free' pointer if necessary.
    if (p_first < p_first_free)
    {
      p_first_free = p_first;
    }
  }

  //*********************************************************************
  // max_size
  // Returns the maximum size that can be allocated in total.
  //*********************************************************************
  size_type max_size() const
  {
      return (MAX_SIZE);
  }

  //*********************************************************************
  // construct
  // Constructs an item.
  //*********************************************************************
  void construct(pointer p, const value_type &x)
  {
    // Placement 'new'
    new (p)value_type(x);
  }

  //*********************************************************************
  // destroy
  // Destroys an item.
  //*********************************************************************
  void destroy(pointer p)
  {
    // Call the destructor.
    p->~value_type();
  }

private:

  enum
  {
    ALIGNMENT = std::alignment_of<T>::value - 1
  };

  //*********************************************************************
  // initialise
  // Initialises the internal allocation buffers.
  //*********************************************************************
  void initialise()
  {
    // Ensure alignment.
    p_buffer = reinterpret_cast<char *>((reinterpret_cast<size_t>(&buffer[0]) + ALIGNMENT) & ~ALIGNMENT);

    // Mark all slots as free.
    std::fill(in_use, in_use + MAX_SIZE, FREE);
  }

  // Disabled operator.
  void operator =(const fixed_allocator &);
  
  // The allocation buffer. Ensure enough space for correct alignment.
  char buffer[(MAX_SIZE * sizeof(value_type)) + ALIGNMENT + 1];

  // Pointer to the first valid location in the buffer after alignment.
  char *p_buffer;

  // The flags that indicate which slots are in use.
  bool in_use[MAX_SIZE];

  // Pointer to the first free slot.
  bool *p_first_free;
};

//*********************************************************************
// operator ==
// Equality operator.
//*********************************************************************
template<typename T, const size_t MAX_SIZE>
inline bool operator ==(const fixed_allocator<T, MAX_SIZE> &,
                        const fixed_allocator<T, MAX_SIZE> &)
{
  return (false);
}

//*********************************************************************
// operator !=
// Inequality operator.
//*********************************************************************
template<typename T, const size_t MAX_SIZE>
inline bool operator !=(const fixed_allocator<T, MAX_SIZE> &, 
                        const fixed_allocator<T, MAX_SIZE> &)
{
  return (true);
}


//**********************************************************************
// fixed_block_allocator
//**********************************************************************
template <typename T, const size_t MAX_SIZE>
class fixed_block_allocator
{
public:
typedef T                  value_type;
  typedef value_type *       pointer;
  typedef const value_type * const_pointer;
  typedef value_type &       reference;
  typedef const value_type & const_reference;
  typedef std::size_t        size_type;
  typedef std::ptrdiff_t     difference_type;

  enum
  {
    NUMBER_OF_BUFFERS = std::is_trivially_destructible<T>::value ? 1 : 2 // The numbers of buffers to use. Varies according to the type.
  };

  //*********************************************************************
  // rebind
  //*********************************************************************
  template<typename U>
  struct rebind
  {
      typedef fixed_block_allocator<U, MAX_SIZE> other;
  };

  //*********************************************************************
  // Constructor
  //*********************************************************************
  fixed_block_allocator()
      : buffer_id(0)
  {
      initialise();
  }

  //*********************************************************************
  // Copy constructor
  //*********************************************************************
  fixed_block_allocator(const fixed_block_allocator &rhs)
      : buffer_id(0)
  {
      initialise();
  }

  //*********************************************************************
  // Templated copy constructor
  //*********************************************************************
  template<typename U>
  fixed_block_allocator(const fixed_block_allocator<U, MAX_SIZE> &rhs)
      : buffer_id(0)
  {
      initialise();
  }

  //*********************************************************************
  // Destructor
  //*********************************************************************
  ~fixed_block_allocator()
  {
  }

  //*********************************************************************
  // address
  //*********************************************************************
  pointer address(reference x) const
  {
      return (&x);
  }

  //*********************************************************************
  // address
  //*********************************************************************
  const_pointer address(const_reference x) const
  {
      return (x);
  }

  //*********************************************************************
  // allocate
  // Allocates from the internal array.
  // If storage cannot be allocated then std::bad_alloc() is thrown.
  //*********************************************************************
  pointer allocate(size_type     n,
                   const_pointer cp = 0)
  {
      // Just too big?
      if (n > MAX_SIZE)
      {
          throw std::bad_alloc();
      }

      // Get the next buffer.
      buffer_id = ++buffer_id % NUMBER_OF_BUFFERS;

      // Always return the beginning of the buffer.
      return (reinterpret_cast<pointer>(p_buffer[buffer_id]));
  }

  //*********************************************************************
  // deallocate
  // Does nothing.
  //*********************************************************************
  void deallocate(pointer   p,
                  size_type n)
  {
  }

  //*********************************************************************
  // max_size
  // Returns the maximum size that can be allocated in total.
  //*********************************************************************
  size_type max_size() const
  {
      return (MAX_SIZE);
  }

  //*********************************************************************
  // construct
  // Constructs an item.
  //*********************************************************************
  void construct(pointer          p,
                 const value_type &x)
  {
      new (p)value_type(x);
  }

  //*********************************************************************
  // destroy
  // Destroys an item.
  //*********************************************************************
  void destroy(pointer p)
  {
      p->~value_type();
  }

private:

  enum
  {
      ALIGNMENT = std::alignment_of<T>::value - 1 // The alignment of the buffers - 1
  };

  //*********************************************************************
  // initialise
  // Initialises the internal allocation buffers.
  //*********************************************************************
  void initialise()
  {
      // Ensure alignment.
      for (int i = 0; i < NUMBER_OF_BUFFERS; ++i)
      {
          p_buffer[i] = reinterpret_cast<char *>((reinterpret_cast<size_t>(&buffer[i][0]) + ALIGNMENT) & ~ALIGNMENT);
      }
  }

  // Disabled operator.
  void operator =(const fixed_block_allocator &);

  // The allocation buffers. Ensure enough space for correct alignment.
  char buffer[NUMBER_OF_BUFFERS][(MAX_SIZE * sizeof(value_type)) + ALIGNMENT + 1];

  // Pointers to the first valid locations in the buffers after alignment.
  char *p_buffer[NUMBER_OF_BUFFERS];

  // The index of the currently allocated buffer.
  int buffer_id;
};

//*********************************************************************
// operator ==
// Equality operator.
//*********************************************************************
template<typename T, const size_t MAX_SIZE>
inline bool operator ==(const fixed_block_allocator<T, MAX_SIZE> &,
                        const fixed_block_allocator<T, MAX_SIZE> &)
{
    return (false);
}

//*********************************************************************
// operator !=
// Inequality operator.
//*********************************************************************
template<typename T, const size_t MAX_SIZE>
inline bool operator !=(const fixed_block_allocator<T, MAX_SIZE> &,
                        const fixed_block_allocator<T, MAX_SIZE> &)
{
    return (true);
}   

#endif